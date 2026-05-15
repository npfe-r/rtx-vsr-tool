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
    if (!m->fmtCtx || !m->decCtx) return false;
    if (!DecodeOne()) return false;

    if (m->useHW && m->decoded->format == m->hwPixFmt) {
        if (av_hwframe_transfer_data(m->swFrame, m->decoded, 0) < 0)
            return false;
        int h = m->targetH;
        int w = m->targetW;
        for (int y = 0; y < h; y++)
            memcpy(outData + y * w,
                   m->swFrame->data[0] + y * m->swFrame->linesize[0], w);
        int uvH = h / 2;
        uint8_t* uvDst = outData + w * h;
        for (int y = 0; y < uvH; y++)
            memcpy(uvDst + y * w,
                   m->swFrame->data[1] + y * m->swFrame->linesize[1], w);
        outStride[0] = w;
        outStride[1] = w;
        return true;
    }
    {
        int dstStrides[2] = { m->targetW, m->targetW };
        uint8_t* dst[2] = { outData, outData + m->targetW * m->targetH };

        m->swsCtx = sws_getCachedContext(m->swsCtx,
            m->decoded->width, m->decoded->height,
            (AVPixelFormat)m->decoded->format,
            m->targetW, m->targetH, AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR, NULL, NULL, NULL);

        sws_scale(m->swsCtx, m->decoded->data, m->decoded->linesize,
                  0, m->decoded->height, dst, dstStrides);

        outStride[0] = m->targetW;
        outStride[1] = m->targetW;
        return true;
    }
}

bool VideoDecoder::ReadFrameGPU(const uint8_t** outY, int* yPitch,
                                 const uint8_t** outUV, int* uvPitch) {
    if (!m->fmtCtx || !m->decCtx) return false;
    if (!DecodeOne()) return false;

    if (m->useHW && m->decoded->format == m->hwPixFmt) {
        *outY = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(m->decoded->data[0]));
        *yPitch = m->decoded->linesize[0];
        *outUV = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(m->decoded->data[1]));
        *uvPitch = m->decoded->linesize[1];
        return true;
    }
    return false;
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
    m_eof = false;
    m_drainSent = false;
}

void VideoDecoder::SetAudioPacketQueue(void* queue) {
    m->audioPackets = static_cast<std::vector<AVPacket*>*>(queue);
}

void* VideoDecoder::GetAudioCodecPar() const {
    return m->audioCodecPar;
}
