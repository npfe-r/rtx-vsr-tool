#include "video_decoder.h"
#include "color_converter.h"
#include "color_types.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <vector>

#include "debug_util.h"

// Map FFmpeg AVColorSpace to our simplified ColorMatrix enum.
// Defaults to BT.709 (the most common for HD video) when unspecified.
static int AvColorSpaceToMatrix(int avCS) {
    switch (avCS) {
        case 5:  // AVCOL_SPC_BT470BG
        case 6:  // AVCOL_SPC_SMPTE170M
            return COLOR_MATRIX_BT601;
        case 1:  // AVCOL_SPC_BT709
            return COLOR_MATRIX_BT709;
        case 9:  // AVCOL_SPC_BT2020_NCL
            return COLOR_MATRIX_BT2020_NCL;
        case 10: // AVCOL_SPC_BT2020_CL (Constant Luminance)
            return COLOR_MATRIX_BT2020_CL;
        default:
            return COLOR_MATRIX_BT709;  // safe default for modern content
    }
}

static AVPixelFormat g_hw_pix_fmt = AV_PIX_FMT_NONE;

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                        const enum AVPixelFormat* pixFmts) {
    const enum AVPixelFormat* p;
    for (p = pixFmts; *p != -1; p++) {
        if (*p == g_hw_pix_fmt)
            return *p;
    }
    return pixFmts[0];
}

struct VideoDecoder::Impl {
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext*  decCtx  = nullptr;
    AVFrame*         decoded = nullptr;
    AVFrame*         swFrame = nullptr;
    SwsContext*      swsCtx  = nullptr;

    AVBufferRef*     hwDeviceCtx = nullptr;
    AVPixelFormat    hwPixFmt = AV_PIX_FMT_NONE;
    bool             useHW = false;

    int videoStreamIdx = -1;
    int targetW = 0, targetH = 0;

    // Audio
    int audioStreamIdx = -1;
    std::vector<AVPacket*>* audioPackets = nullptr;
    AVCodecParameters* audioCodecPar = nullptr;

    // PTS reorder buffer (GPU decode path)
    // Decodes up to m_reorderDepth frames, then returns earliest PTS (= display order).
    // Uses av_frame_ref for zero-copy NVDEC surface retention.
    // m_reorderAges[i] tracks how many times the corresponding frame was
    // passed over (not selected) — prevents invalid-PTS frames from starving.
    std::vector<AVFrame*> m_reorderBuffer;
    std::vector<int>      m_reorderAges;
    AVFrame* m_gpuOutputFrame = nullptr;
    int64_t  m_lastPTS = -1;            // PTS of the last decoded frame (CPU path)
    static const int m_reorderDepth = 8;
};

VideoDecoder::VideoDecoder() : m(new Impl) {}
VideoDecoder::~VideoDecoder() { Close(); delete m; }

bool VideoDecoder::IsOpen() const { return m->fmtCtx != nullptr; }
bool VideoDecoder::IsHWDecoding() const { return m->useHW; }

// ---- Shared decode loop: fills m->decoded with the next frame, returns true on success ----
bool VideoDecoder::DecodeOne() {
    if (!m_drainSent) {
        int ret = avcodec_receive_frame(m->decCtx, m->decoded);
        if (ret == 0) return true;
    }
    if (m_drainSent) return false;

    if (!m_eof) {
        AVPacket pkt;
        while (av_read_frame(m->fmtCtx, &pkt) >= 0) {
            int si = pkt.stream_index;
            if (si == m->videoStreamIdx) {
                int sr = avcodec_send_packet(m->decCtx, &pkt);
                av_packet_unref(&pkt);
                if (sr < 0) continue;
                while (avcodec_receive_frame(m->decCtx, m->decoded) >= 0)
                    return true;
            } else if (m->audioPackets && si == m->audioStreamIdx) {
                AVPacket* copy = av_packet_alloc();
                av_packet_ref(copy, &pkt);
                m->audioPackets->push_back(copy);
                av_packet_unref(&pkt);
            } else {
                av_packet_unref(&pkt);
            }
        }
        m_eof = true;
    }

    if (m_eof && !m_drainSent) {
        avcodec_send_packet(m->decCtx, NULL);
        m_drainSent = true;
        return avcodec_receive_frame(m->decCtx, m->decoded) >= 0;
    }

    return false;
}

bool VideoDecoder::Open(const wchar_t* path, VideoInfo* info, bool useGPU) {
    Close();

    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, NULL, NULL);

    AVDictionary* opts = nullptr;
    av_dict_set_int(&opts, "probesize", 100 * 1024 * 1024, 0);
    av_dict_set_int(&opts, "analyzeduration", 30 * AV_TIME_BASE, 0);

    if (avformat_open_input(&m->fmtCtx, pathA, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(m->fmtCtx, NULL) < 0) {
        avformat_close_input(&m->fmtCtx);
        return false;
    }

    // Find video stream
    const AVCodec* codec = nullptr;
    m->videoStreamIdx = av_find_best_stream(m->fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m->videoStreamIdx < 0) return false;

    AVStream* vs = m->fmtCtx->streams[m->videoStreamIdx];
    m->decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m->decCtx, vs->codecpar);

    // Try to initialize CUDA hardware decoding
    if (useGPU) {
        enum AVHWDeviceType hwType = av_hwdevice_find_type_by_name("cuda");
        if (hwType != AV_HWDEVICE_TYPE_NONE) {
            for (int i = 0;; i++) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                if (!config) break;
                if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                    config->device_type == hwType) {
                    m->hwPixFmt = config->pix_fmt;
                    break;
                }
            }
        }
        if (m->hwPixFmt != AV_PIX_FMT_NONE) {
            AVBufferRef* hwCtx = nullptr;
            if (av_hwdevice_ctx_create(&hwCtx, hwType, NULL, NULL, 0) >= 0) {
                m->hwDeviceCtx = hwCtx;
                m->useHW = true;
                m->decCtx->hw_device_ctx = av_buffer_ref(m->hwDeviceCtx);
                g_hw_pix_fmt = m->hwPixFmt;
                m->decCtx->get_format = get_hw_format;
            }
        }
        if (m->useHW) {
            LogMsg("DEC: ","CUDA hardware decoding enabled");
        } else {
            LogMsg("DEC: ","CUDA hardware decoding not available, falling back to software");
        }
    }

    if (avcodec_open2(m->decCtx, codec, NULL) < 0) return false;

    m->decoded = av_frame_alloc();
    if (m->useHW)
        m->swFrame = av_frame_alloc();
    m->targetW = m->decCtx->width;
    m->targetH = m->decCtx->height;

    // Fill VideoInfo
    info->width  = m->decCtx->width;
    info->height = m->decCtx->height;
    {
        AVRational r = av_guess_frame_rate(m->fmtCtx, vs, NULL);
        info->fps = r.den > 0 ? av_q2d(r) : 30.0;
    }
    info->totalFrames = vs->nb_frames > 0 ? (int)vs->nb_frames
                        : (int)(m->fmtCtx->duration * info->fps / AV_TIME_BASE + 1);
    if (info->totalFrames < 0) info->totalFrames = 0;
    info->hasVideo = true;
    info->srcTimeBaseNum = vs->time_base.num;
    info->srcTimeBaseDen = vs->time_base.den;

    if (codec)
        strncpy(info->videoCodecName, codec->name, sizeof(info->videoCodecName) - 1);

    // Find audio stream
    m->audioStreamIdx = av_find_best_stream(m->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, m->videoStreamIdx, NULL, 0);
    if (m->audioStreamIdx >= 0) {
        AVStream* audioStream = m->fmtCtx->streams[m->audioStreamIdx];
        info->audioStreamIndex = m->audioStreamIdx;
        info->audioSampleRate = audioStream->codecpar->sample_rate;
        info->audioChannels = audioStream->codecpar->ch_layout.nb_channels;
        info->hasAudio = true;

        const AVCodec* audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (audioCodec)
            strncpy(info->audioCodecName, audioCodec->name, sizeof(info->audioCodecName) - 1);

        // Copy codecpar for encoder remux
        m->audioCodecPar = avcodec_parameters_alloc();
        if (m->audioCodecPar) {
            avcodec_parameters_copy(m->audioCodecPar, audioStream->codecpar);
        }
    }

    m_eof = false;
    m_drainSent = false;

    // ── Read source colour metadata ────────────────────────────────
    info->avColorPrimaries = m->decCtx->color_primaries;
    info->avColorTransfer  = m->decCtx->color_trc;
    info->avColorSpace     = m->decCtx->colorspace;
    info->avColorRange     = m->decCtx->color_range;
    info->srcColorMatrix   = AvColorSpaceToMatrix(m->decCtx->colorspace);
    {
        const char* clSuffix = "";
        if (m->decCtx->colorspace == 10) clSuffix = " (BT.2020 CL — using CL coefficients)";
        char _b[256]; snprintf(_b, sizeof(_b),
            "DEC: color: primaries=%d transfer=%d space=%d range=%d  → matrix=%d%s",
            info->avColorPrimaries, info->avColorTransfer,
            info->avColorSpace, info->avColorRange,
            AvColorSpaceToMatrix(info->avColorSpace), clSuffix);
        LogMsg("DEC: ",_b);
    }

    return true;
}

bool VideoDecoder::ReadFrameNV12(uint8_t* outData, int* outStride) {
    if (!m->fmtCtx || !m->decCtx) return false;

    if (!DecodeOne()) return false;

    int w = m->targetW;
    int h = m->targetH;

    if (!m->swsCtx) {
        int srcCS = AvColorSpaceToSWS(m->decCtx->colorspace);
        m->swsCtx = sws_getContext(w, h, m->decCtx->pix_fmt,
                                    w, h, AV_PIX_FMT_NV12,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (m->swsCtx) {
            int srcRange = (m->decCtx->color_range == 2) ? 1 : 0;  // 1 = full range
            // Destination NV12 preserves the source colour matrix so the
            // downstream CUDA kernel (which selects the same matrix) can
            // convert back to RGB without a matrix mismatch.
            sws_setColorspaceDetails(m->swsCtx,
                sws_getCoefficients(srcCS), srcRange,
                sws_getCoefficients(srcCS), 0,   // dst = same matrix, limited
                0, 1 << 16, 1 << 16);
        }
        if (!m->swsCtx) return false;
    }

    uint8_t* dst[2] = { outData, outData + w * h };
    int dstStride[2] = { w, w };
    sws_scale(m->swsCtx, m->decoded->data, m->decoded->linesize, 0, h, dst, dstStride);

    if (outStride) *outStride = w;

    m->m_lastPTS = m->decoded->pts;
    return true;
}

bool VideoDecoder::ReadFrameGPU(const uint8_t** outY, int* yPitch,
                                 const uint8_t** outUV, int* uvPitch,
                                 int64_t* outPTS) {
    if (!m->fmtCtx || !m->decCtx) return false;

    // Fill reorder buffer: keep decoding until buffer is full or EOF is drained
    while (m->m_reorderBuffer.size() < m->m_reorderDepth && !(m_eof && m_drainSent)) {
        if (!DecodeOne()) {
            // EOF reached or no more frames to drain — will use whatever is in the buffer
            continue;
        }

        AVFrame* held = av_frame_alloc();
        if (!held) break;
        if (av_frame_ref(held, m->decoded) < 0) {
            av_frame_free(&held);
            break;
        }
        m->m_reorderBuffer.push_back(held);
        m->m_reorderAges.push_back(0);
    }

    if (m->m_reorderBuffer.empty()) return false;

    // Select the frame with earliest PTS (= display order).
    // Frames with valid PTS (>=0) sort before invalid PTS (-1 / AV_NOPTS_VALUE).
    // Among valid PTS, smaller value = earlier display position.
    // Among invalid PTS, the one that was decoded first (remained in buffer longest)
    // is output first, preserving at least decode-order determinism.
    //
    // Starvation guard: an invalid-PTS frame that has been passed over more than
    // m_reorderDepth * 2 times is force-selected.  This prevents a small number
    // of PTS-less frames from being perpetually delayed by a steady stream of
    // valid-PTS frames, which manifests as a "stray old frame" ~1 s into output.
    int bestIdx = 0;
    {
        const int kMaxReorderAge = m->m_reorderDepth * 2; // ~16 frames
        bool forced = false;
        for (size_t i = 0; i < m->m_reorderBuffer.size(); i++) {
            if (m->m_reorderAges[i] > kMaxReorderAge &&
                m->m_reorderBuffer[i]->pts < 0) {
                bestIdx = i;
                forced = true;
                break;
            }
        }
        if (!forced) {
            for (size_t i = 1; i < m->m_reorderBuffer.size(); i++) {
                int64_t pi = m->m_reorderBuffer[i]->pts;
                int64_t pb = m->m_reorderBuffer[bestIdx]->pts;

                bool iBetter = false;
                if (pi >= 0 && pb < 0) {
                    iBetter = true;
                } else if (pi >= 0 && pb >= 0) {
                    iBetter = pi < pb;
                }  // else both invalid: keep bestIdx (smallest index = earliest decode order)

                if (iBetter) bestIdx = i;
            }
        }
    }

    AVFrame* selected = m->m_reorderBuffer[bestIdx];
    m->m_reorderBuffer.erase(m->m_reorderBuffer.begin() + bestIdx);
    m->m_reorderAges.erase(m->m_reorderAges.begin() + bestIdx);

    // Age the remaining frames: each non-selected frame records one pass-over.
    // Used by the starvation guard on future ReadFrameGPU calls.
    for (int& age : m->m_reorderAges) age++;

    // GPU device pointers from NVDEC surface — valid as long as the AVFrame ref lives.
    *outY = reinterpret_cast<const uint8_t*>(
        reinterpret_cast<uintptr_t>(selected->data[0]));
    *yPitch = selected->linesize[0];
    *outUV = reinterpret_cast<const uint8_t*>(
        reinterpret_cast<uintptr_t>(selected->data[1]));
    *uvPitch = selected->linesize[1];
    if (outPTS) *outPTS = selected->pts;

    // Retain a ref so the NVDEC surface stays alive until the next ReadFrameGPU call
    // (the caller must cudaStreamSynchronize / cudaEventSynchronize before then).
    if (m->m_gpuOutputFrame) av_frame_free(&m->m_gpuOutputFrame);
    m->m_gpuOutputFrame = selected;

    return true;
}

void VideoDecoder::Close() {
    if (m->swsCtx) sws_freeContext(m->swsCtx);
    if (m->swFrame) av_frame_free(&m->swFrame);
    if (m->decoded) av_frame_free(&m->decoded);
    if (m->hwDeviceCtx) av_buffer_unref(&m->hwDeviceCtx);
    if (m->decCtx) avcodec_free_context(&m->decCtx);
    if (m->fmtCtx) avformat_close_input(&m->fmtCtx);
    if (m->audioCodecPar) { avcodec_parameters_free(&m->audioCodecPar); m->audioCodecPar = nullptr; }
    m->videoStreamIdx = -1;
    m->audioStreamIdx = -1;
    m->swsCtx = nullptr;
    m->useHW = false;
    m->hwPixFmt = AV_PIX_FMT_NONE;
    // Cleanup PTS reorder buffer
    for (auto* frame : m->m_reorderBuffer) {
        if (frame) av_frame_free(&frame);
    }
    m->m_reorderBuffer.clear();
    m->m_reorderAges.clear();
    if (m->m_gpuOutputFrame) {
        av_frame_free(&m->m_gpuOutputFrame);
        m->m_gpuOutputFrame = nullptr;
    }

    m_eof = false;
    m_drainSent = false;
}

void VideoDecoder::SetAudioPacketQueue(void* queue) {
    m->audioPackets = static_cast<std::vector<AVPacket*>*>(queue);
}

void* VideoDecoder::GetAudioCodecPar() const {
    return m->audioCodecPar;
}

int64_t VideoDecoder::GetLastPTS() const {
    if (m->m_gpuOutputFrame)
        return m->m_gpuOutputFrame->pts;
    return m->m_lastPTS;
}
