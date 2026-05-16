#include "video_decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <vector>

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
    std::vector<AVFrame*> m_reorderBuffer;
    AVFrame* m_gpuOutputFrame = nullptr;
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
            OutputDebugStringA("DEC: CUDA hardware decoding enabled\n");
        } else {
            OutputDebugStringA("DEC: CUDA hardware decoding not available, falling back to software\n");
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

    return true;
}

bool VideoDecoder::ReadFrameNV12(uint8_t* outData, int* outStride) {
    // CPU decode path is disabled in GPU-only pipeline mode
    (void)outData;
    (void)outStride;
    return false;
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
    }

    if (m->m_reorderBuffer.empty()) return false;

    // Select the frame with earliest PTS (= display order).
    // Frames with valid PTS (>=0) sort before invalid PTS (-1 / AV_NOPTS_VALUE).
    // Among valid PTS, smaller value = earlier display position.
    // Among invalid PTS, the one that was decoded first (remained in buffer longest)
    // is output first, preserving at least decode-order determinism.
    int bestIdx = 0;
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

    AVFrame* selected = m->m_reorderBuffer[bestIdx];
    m->m_reorderBuffer.erase(m->m_reorderBuffer.begin() + bestIdx);

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
    return m->m_gpuOutputFrame ? m->m_gpuOutputFrame->pts : -1;
}
