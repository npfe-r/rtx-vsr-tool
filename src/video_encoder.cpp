#include "video_encoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <thread>
#include <windows.h>

static void EncLog(const char* msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "ENC: %s\n", msg); fflush(f); fclose(f); }

    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, "ENC: ", 5, &wrote, nullptr);
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}

struct VideoEncoder::Impl {
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext*  encCtx  = nullptr;
    AVStream*        videoStream = nullptr;
    AVFrame*         frame   = nullptr;
    AVPixelFormat    pixFmt  = AV_PIX_FMT_NV12;
    bool encCtxFailed = false;
    int frameCount = 0;

    // Audio remux (fallback)
    AVStream* audioStream = nullptr;
    int audioStreamIdx = -1;
    std::vector<AVPacket*>* audioPackets = nullptr;

    // Audio transcode
    AVCodecContext* audioDecCtx = nullptr;  // source audio decoder
    AVCodecContext* audioEncCtx = nullptr;  // AAC encoder
    AVFrame*        audioFrame = nullptr;   // decoded PCM frame
    AVFrame*        audioEncFrame = nullptr; // resampled frame for encoder
    SwrContext*     audioSwr = nullptr;
    bool            audioTranscoding = false;

    SwsContext*     swsCtx = nullptr;

    // CUDA hwcontext for NVENC zero-copy GPU encoding
    AVBufferRef*     hwDeviceCtx = nullptr;
    AVBufferRef*     hwFramesCtx = nullptr;
    AVFrame*         cudaFrame   = nullptr;
    bool             useCUDA     = false;
};

static const char* GetEncoderName(int id) {
    switch (id) {
        case 0:  return "h264_nvenc";
        case 1:  return "hevc_nvenc";
        case 2:  return "av1_nvenc";
        case 3:  return "libx264";
        case 4:  return "libx265";
        case 5:  return "libaom-av1";
        default: return "h264_nvenc";
    }
}

static bool IsNVENCByName(const char* name) {
    return strcmp(name, "h264_nvenc") == 0 ||
           strcmp(name, "hevc_nvenc") == 0 ||
           strcmp(name, "av1_nvenc") == 0;
}

static const char* GetDisplayName(const char* name) {
    if (strcmp(name, "h264_nvenc") == 0) return "H.264 NVENC";
    if (strcmp(name, "hevc_nvenc") == 0) return "HEVC NVENC";
    if (strcmp(name, "av1_nvenc") == 0) return "AV1 NVENC";
    if (strcmp(name, "libx264") == 0) return "libx264";
    if (strcmp(name, "libx265") == 0) return "libx265";
    if (strcmp(name, "libaom-av1") == 0) return "libaom-av1";
    return name;
}

static const char* GetContainerExt(int container) {
    switch (container) {
        case 0: return "mp4";
        case 1: return "mov";
        default: return "mp4";
    }
}

// Per-codec NVENC maximum dimensions
static const int H264_NVENC_MAX_WIDTH  = 4096;
static const int H264_NVENC_MAX_HEIGHT = 4096;
static const int HEVC_NVENC_MAX_WIDTH  = 8192;
static const int HEVC_NVENC_MAX_HEIGHT = 8192;
static const int AV1_NVENC_MAX_WIDTH   = 8192;
static const int AV1_NVENC_MAX_HEIGHT  = 8192;

static bool IsResolutionWithinNVENC(int codecId, int width, int height) {
    switch (codecId) {
        case 0: return width <= H264_NVENC_MAX_WIDTH && height <= H264_NVENC_MAX_HEIGHT;
        case 1: return width <= HEVC_NVENC_MAX_WIDTH && height <= HEVC_NVENC_MAX_HEIGHT;
        case 2: return width <= AV1_NVENC_MAX_WIDTH  && height <= AV1_NVENC_MAX_HEIGHT;
        default: return true;
    }
}

VideoEncoder::VideoEncoder() : m(new Impl) {}
VideoEncoder::~VideoEncoder() { Close(); delete m; }
bool VideoEncoder::IsOpen() const { return m->fmtCtx != nullptr; }

bool VideoEncoder::Open(const EncodeConfig& cfg, OnEncoderStatus statusCb) {
    Close();

    { char _b[256]; snprintf(_b, sizeof(_b), "Open encoder: codecId=%d width=%d height=%d fps=%.1f crf=%d speed=%d container=%d",
        cfg.codecId, cfg.width, cfg.height, cfg.fps, cfg.crf, cfg.speed, cfg.container); EncLog(_b); }

    const char* encName = GetEncoderName(cfg.codecId);
    const char* displayName = GetDisplayName(encName);

    bool nvencOversize = (cfg.codecId <= 2) && !IsResolutionWithinNVENC(cfg.codecId, cfg.width, cfg.height);
    if (nvencOversize) {
        if (statusCb) {
            char msg[256];
            int maxW = 8192, maxH = 8192;
            if (cfg.codecId == 0)      { maxW = H264_NVENC_MAX_WIDTH;  maxH = H264_NVENC_MAX_HEIGHT; }
            else if (cfg.codecId == 1) { maxW = HEVC_NVENC_MAX_WIDTH;  maxH = HEVC_NVENC_MAX_HEIGHT; }
            else if (cfg.codecId == 2) { maxW = AV1_NVENC_MAX_WIDTH;   maxH = AV1_NVENC_MAX_HEIGHT; }
            snprintf(msg, sizeof(msg), "编码器 %s 打开失败: 分辨率(%dx%d)超出上限(%dx%d)",
                     displayName, cfg.width, cfg.height, maxW, maxH);
            statusCb(msg);
        }
        return false;
    }

    if (statusCb) {
        char msg[128];
        snprintf(msg, sizeof(msg), "编码器: 打开 %s...", displayName);
        statusCb(msg);
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(encName);
    if (!codec) {
        if (statusCb) {
            char msg[128];
            snprintf(msg, sizeof(msg), "编码器 %s 未找到", displayName);
            statusCb(msg);
        }
        return false;
    }

    const char* ext = GetContainerExt(cfg.container);
    avformat_alloc_output_context2(&m->fmtCtx, NULL, ext, NULL);
    if (!m->fmtCtx) { EncLog("avformat_alloc_output_context2 failed"); return false; }
    m->videoStream = avformat_new_stream(m->fmtCtx, NULL);

    m->encCtx = avcodec_alloc_context3(codec);
    m->encCtx->width     = cfg.width;
    m->encCtx->height    = cfg.height;
    AVRational fpsRat = av_d2q(cfg.fps, 1001);
    m->encCtx->time_base = av_inv_q(fpsRat);
    m->encCtx->framerate = fpsRat;
    m->encCtx->pix_fmt   = m->pixFmt = IsNVENCByName(encName) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    m->encCtx->color_range = AVCOL_RANGE_MPEG;
    m->encCtx->color_primaries = AVCOL_PRI_BT709;
    m->encCtx->color_trc = AVCOL_TRC_BT709;
    m->encCtx->colorspace = AVCOL_SPC_BT709;
    m->encCtx->chroma_sample_location = AVCHROMA_LOC_LEFT;

    if (IsNVENCByName(encName)) {
        const char* nvPresets[] = { "p1", "p3", "p4", "p6", "p7" };
        int idx = cfg.speed;
        if (idx < 0) idx = 0;
        if (idx > 4) idx = 4;
        av_opt_set(m->encCtx->priv_data, "preset", nvPresets[idx], 0);
        av_opt_set_int(m->encCtx->priv_data, "cq", cfg.crf, 0);
        av_opt_set(m->encCtx->priv_data, "rc", "vbr", 0);
        av_opt_set_int(m->encCtx->priv_data, "b", 0, 0);
        // Disable B-frames: NVENC auto-default (bf=-1) enables them for presets
        // P3-P7, which causes the encoder to internally reorder frames. With
        // the pipeline's single-send-per-call pattern this produces output with
        // wrong frame timing. bf=0 ensures PTS == DTS for every frame.
        av_opt_set_int(m->encCtx->priv_data, "bf", 0, 0);
        // Disable lookahead — prevent NVENC from buffering frames internally
        // for rate-control analysis. Lookahead causes the encoder to hold
        // references across send_frame/receive_packet boundaries, which can
        // crash when the pipeline reuses m->frame for every call (notably
        // with AV1 NVENC around frame 22).
        av_opt_set_int(m->encCtx->priv_data, "rc-lookahead", 0, 0);

        // CUDA hwcontext for NVENC zero-copy GPU encoding.
        // Creates a CUDA device context + hwframe pool so the encoder can accept
        // AV_PIX_FMT_CUDA frames with device pointers directly — no D2H + memcpy.
        {
            AVBufferRef* hwDev = nullptr;
            if (av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_CUDA,
                                       nullptr, nullptr, 0) >= 0) {
                m->hwDeviceCtx = hwDev;

                AVBufferRef* hwfc_ref = av_hwframe_ctx_alloc(m->hwDeviceCtx);
                AVHWFramesContext* hwfc = (AVHWFramesContext*)hwfc_ref->data;
                hwfc->format    = AV_PIX_FMT_CUDA;
                hwfc->sw_format = AV_PIX_FMT_NV12;
                hwfc->width     = cfg.width;
                hwfc->height    = cfg.height;
                hwfc->initial_pool_size = 4;
                if (av_hwframe_ctx_init(hwfc_ref) >= 0) {
                    m->hwFramesCtx = hwfc_ref;
                    m->cudaFrame = av_frame_alloc();
                    m->encCtx->pix_fmt = AV_PIX_FMT_CUDA;
                    m->pixFmt = AV_PIX_FMT_CUDA;
                    m->useCUDA = true;
                    m->encCtx->hw_device_ctx = av_buffer_ref(m->hwDeviceCtx);
                    m->encCtx->hw_frames_ctx = av_buffer_ref(m->hwFramesCtx);
                    if (statusCb)
                        statusCb("编码器: CUDA 零拷贝编码已启用");
                } else {
                    EncLog("av_hwframe_ctx_init failed for CUDA hwframe pool");
                    av_buffer_unref(&m->hwDeviceCtx);
                    m->hwDeviceCtx = nullptr;
                }
            } else {
                EncLog("av_hwdevice_ctx_create failed for CUDA");
            }
        }
    } else {
        char crfStr[8];
        snprintf(crfStr, sizeof(crfStr), "%d", cfg.crf);
        av_opt_set(m->encCtx->priv_data, "crf", crfStr, 0);

        int cpuUsed = 0;
        const char* usage = "good";
        switch (cfg.speed) {
            case 0: cpuUsed = 6; usage = "realtime"; break;
            case 1: cpuUsed = 5; usage = "realtime"; break;
            case 2: cpuUsed = 3; usage = "good";     break;
            case 3: cpuUsed = 1; usage = "good";     break;
            case 4: cpuUsed = 0; usage = "good";     break;
        }

        m->encCtx->thread_count = (int)std::thread::hardware_concurrency();
        av_opt_set_int(m->encCtx->priv_data, "cpu-used", cpuUsed, 0);
        av_opt_set(m->encCtx->priv_data, "usage", usage, 0);
        av_opt_set_int(m->encCtx->priv_data, "row-mt", 1, 0);
        av_opt_set_int(m->encCtx->priv_data, "lag-in-frames", 0, 0);

        // Disable B-frames for software encoders — same rationale as NVENC bf=0
        // above: the pipeline sends one frame at a time and expects the encoder
        // to output the packet immediately. B-frames would cause internal
        // reordering and produce wrong frame timing.
        av_opt_set_int(m->encCtx->priv_data, "bf", 0, 0);      // libx264
        av_opt_set_int(m->encCtx->priv_data, "bframes", 0, 0); // libx265
        av_opt_set_int(m->encCtx->priv_data, "arnr-maxframes", 0, 0); // libaom-av1 alt-ref
        av_opt_set_int(m->encCtx->priv_data, "arnr-strength", 0, 0);  // libaom-av1 alt-ref

        int tileCols = 0, tileRows = 0;
        if (cfg.width >= 3840) tileCols = 1;
        if (cfg.height >= 2160) tileRows = 1;
        av_opt_set_int(m->encCtx->priv_data, "tile-columns", tileCols, 0);
        av_opt_set_int(m->encCtx->priv_data, "tile-rows", tileRows, 0);
    }

    int ret = avcodec_open2(m->encCtx, codec, NULL);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        if (statusCb) {
            char msg[256];
            snprintf(msg, sizeof(msg), "编码器 %s 打开失败: %s", displayName, err);
            statusCb(msg);
        }
        avcodec_free_context(&m->encCtx);
        m->encCtx = nullptr;
        return false;
    }

    if (statusCb) {
        char msg[128];
        snprintf(msg, sizeof(msg), "编码器: %s 打开成功", displayName);
        statusCb(msg);
    }

    avcodec_parameters_from_context(m->videoStream->codecpar, m->encCtx);
    m->videoStream->time_base = m->encCtx->time_base;

    // Initialize swscale for NV12→YUV420P conversion (software encoders only)
    if (m->pixFmt == AV_PIX_FMT_YUV420P) {
        m->swsCtx = sws_getContext(
            cfg.width, cfg.height, AV_PIX_FMT_NV12,
            cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, NULL, NULL, NULL);
    }

    // Audio stream: copy source or transcode to AAC
    if (cfg.hasAudio && cfg.audioMode > 0 && cfg.audioPackets && cfg.audioStreamIdx >= 0 && cfg.audioCodecPar) {
        m->audioPackets = static_cast<std::vector<AVPacket*>*>(cfg.audioPackets);
        AVCodecParameters* srcPar = static_cast<AVCodecParameters*>(cfg.audioCodecPar);

        if (cfg.audioMode == 2) {
            // AAC transcode
            const AVCodec* decCodec = avcodec_find_decoder(srcPar->codec_id);
            if (decCodec) {
                m->audioDecCtx = avcodec_alloc_context3(decCodec);
                if (avcodec_parameters_to_context(m->audioDecCtx, srcPar) >= 0 &&
                    avcodec_open2(m->audioDecCtx, decCodec, NULL) >= 0) {

                    const AVCodec* encCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
                    if (encCodec) {
                        m->audioEncCtx = avcodec_alloc_context3(encCodec);
                        m->audioEncCtx->sample_rate = m->audioDecCtx->sample_rate;
                        m->audioEncCtx->ch_layout   = m->audioDecCtx->ch_layout;
                        m->audioEncCtx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
                        m->audioEncCtx->bit_rate    = cfg.audioBitrate * 1000;

                        if (avcodec_open2(m->audioEncCtx, encCodec, NULL) >= 0) {
                            m->audioSwr = swr_alloc();
                            av_opt_set_chlayout(m->audioSwr, "in_chlayout",  &m->audioDecCtx->ch_layout, 0);
                            av_opt_set_int(m->audioSwr, "in_sample_rate",     m->audioDecCtx->sample_rate, 0);
                            av_opt_set_sample_fmt(m->audioSwr, "in_sample_fmt", m->audioDecCtx->sample_fmt, 0);
                            av_opt_set_chlayout(m->audioSwr, "out_chlayout", &m->audioEncCtx->ch_layout, 0);
                            av_opt_set_int(m->audioSwr, "out_sample_rate",    m->audioEncCtx->sample_rate, 0);
                            av_opt_set_sample_fmt(m->audioSwr, "out_sample_fmt", m->audioEncCtx->sample_fmt, 0);
                            if (swr_init(m->audioSwr) >= 0) {
                                m->audioFrame = av_frame_alloc();
                                m->audioEncFrame = av_frame_alloc();
                                m->audioEncFrame->format     = m->audioEncCtx->sample_fmt;
                                m->audioEncFrame->ch_layout  = m->audioEncCtx->ch_layout;
                                m->audioEncFrame->sample_rate = m->audioEncCtx->sample_rate;

                                m->audioStream = avformat_new_stream(m->fmtCtx, NULL);
                                avcodec_parameters_from_context(m->audioStream->codecpar, m->audioEncCtx);
                                AVRational audio_tb = { 1, m->audioEncCtx->sample_rate };
                                m->audioStream->time_base = audio_tb;
                            }
                            m->audioTranscoding = true;
                        }
                    }
                }
            }
        }

        // Copy source (mode 1) or fallback for mode 2 if transcode failed
        if (!m->audioTranscoding) {
            m->audioStream = avformat_new_stream(m->fmtCtx, NULL);
            avcodec_parameters_copy(m->audioStream->codecpar, srcPar);
            m->audioStream->time_base = srcPar->sample_rate > 0
                ? AVRational{ 1, srcPar->sample_rate }
                : AVRational{ 1, 48000 };
        }
    }

    if (!(m->fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        char pathA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, cfg.outputPath, -1, pathA, MAX_PATH, NULL, NULL);
        { char _b[512]; snprintf(_b, sizeof(_b), "avio_open(%s)...", pathA); EncLog(_b); }
        if (avio_open(&m->fmtCtx->pb, pathA, AVIO_FLAG_WRITE) < 0) {
            EncLog("avio_open failed");
            return false;
        }
        EncLog("avio_open OK");
    }

    avformat_write_header(m->fmtCtx, NULL);

    // Allocate CPU encoding frame (CUDA path uses m->cudaFrame from hwframe pool instead)
    if (!m->useCUDA) {
        m->frame = av_frame_alloc();
        m->frame->width  = cfg.width;
        m->frame->height = cfg.height;
        m->frame->format = m->pixFmt;
        if (av_frame_get_buffer(m->frame, 0) < 0) {
            EncLog("av_frame_get_buffer failed");
            return false;
        }
    }

    return true;
}

bool VideoEncoder::WriteFrameNV12(const uint8_t* data, int yStride, int uvStride, int64_t pts) {
    if (!m->frame || !m->encCtx) return false;

    if (m->pixFmt == AV_PIX_FMT_YUV420P && m->swsCtx) {
        uint8_t* srcData[2] = {
            const_cast<uint8_t*>(data),
            const_cast<uint8_t*>(data + yStride * m->encCtx->height)
        };
        int srcLinesizes[2] = { yStride, uvStride };
        sws_scale(m->swsCtx, srcData, srcLinesizes, 0, m->encCtx->height,
                  m->frame->data, m->frame->linesize);
    } else {
        uint8_t* yDst = m->frame->data[0];
        int yLineSize = m->encCtx->width;
        for (int y = 0; y < m->encCtx->height; y++)
            memcpy(yDst + y * m->frame->linesize[0], data + y * yStride, yLineSize);

        uint8_t* uvDst = m->frame->data[1];
        int uvLineSize = m->encCtx->width;
        int uvHeight = m->encCtx->height / 2;
        for (int y = 0; y < uvHeight; y++)
            memcpy(uvDst + y * m->frame->linesize[1],
                   data + yStride * m->encCtx->height + y * uvStride, uvLineSize);
    }

    m->frame->pts = pts;

    __try {
        if (avcodec_send_frame(m->encCtx, m->frame) < 0) {
            m->encCtxFailed = true;
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EncLog("SEH in avcodec_send_frame");
        m->encCtxFailed = true;
        return false;
    }

    AVPacket pkt = { 0 };
    while (true) {
        __try {
            int ret = avcodec_receive_packet(m->encCtx, &pkt);
            if (ret < 0) break;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in avcodec_receive_packet");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        __try {
            av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
            pkt.stream_index = m->videoStream->index;
            av_interleaved_write_frame(m->fmtCtx, &pkt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in av_interleaved_write_frame");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        av_packet_unref(&pkt);
    }
    return true;
}

bool VideoEncoder::GetFrameBuffer(uint8_t** y, int* yPitch, uint8_t** uv, int* uvPitch) {
    if (!m->useCUDA || !m->hwFramesCtx || !m->cudaFrame) return false;

    if (m->cudaFrame->buf[0])
        av_frame_unref(m->cudaFrame);

    // av_hwframe_get_buffer internally sets frame->hw_frames_ctx
    if (av_hwframe_get_buffer(m->hwFramesCtx, m->cudaFrame, 0) < 0) {
        EncLog("GetFrameBuffer: av_hwframe_get_buffer failed");
        return false;
    }

    *y       = (uint8_t*)m->cudaFrame->data[0];
    *yPitch  = m->cudaFrame->linesize[0];
    *uv      = (uint8_t*)m->cudaFrame->data[1];
    *uvPitch = m->cudaFrame->linesize[1];

    return true;
}

bool VideoEncoder::SubmitFrame(int64_t pts) {
    if (!m->useCUDA || !m->cudaFrame || !m->cudaFrame->buf[0]) return false;

    m->cudaFrame->pts = pts;

    __try {
        int ret = avcodec_send_frame(m->encCtx, m->cudaFrame);
        if (ret < 0) {
            char _e[256]; av_strerror(ret, _e, sizeof(_e));
            char _b[256]; snprintf(_b, sizeof(_b), "SubmitFrame: avcodec_send_frame failed: %s", _e); EncLog(_b);
            m->encCtxFailed = true;
            av_frame_unref(m->cudaFrame);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EncLog("SEH in avcodec_send_frame (CUDA)");
        m->encCtxFailed = true;
        av_frame_unref(m->cudaFrame);
        return false;
    }

    AVPacket pkt = { 0 };
    while (true) {
        int ret;
        __try {
            ret = avcodec_receive_packet(m->encCtx, &pkt);
            if (ret < 0) break;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in avcodec_receive_packet (CUDA)");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        __try {
            av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
            pkt.stream_index = m->videoStream->index;
            av_interleaved_write_frame(m->fmtCtx, &pkt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in av_interleaved_write_frame (CUDA)");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        av_packet_unref(&pkt);
    }

    av_frame_unref(m->cudaFrame);
    return true;
}

void VideoEncoder::Close() {
    if (m->encCtx && m->videoStream && m->fmtCtx && m->fmtCtx->pb && !m->encCtxFailed) {
        __try {
            avcodec_send_frame(m->encCtx, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in Close flush send_frame");
        }
        __try {
            AVPacket pkt = { 0 };
            while (avcodec_receive_packet(m->encCtx, &pkt) >= 0) {
                av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
                pkt.stream_index = m->videoStream->index;
                av_interleaved_write_frame(m->fmtCtx, &pkt);
                av_packet_unref(&pkt);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            EncLog("SEH in Close flush receive_packet");
        }
    }

    // Audio (transcode or remux) - only if fmtCtx was fully opened
    if (m->fmtCtx && m->fmtCtx->pb && m->audioStream && m->audioPackets) {
        if (m->audioTranscoding) {
            AVPacket* pkt = av_packet_alloc();
            for (AVPacket* apkt : *m->audioPackets) {
                if (!apkt) continue;
                avcodec_send_packet(m->audioDecCtx, apkt);
                while (avcodec_receive_frame(m->audioDecCtx, m->audioFrame) >= 0) {
                    int dstSamples = av_rescale_rnd(
                        swr_get_delay(m->audioSwr, m->audioDecCtx->sample_rate) +
                            m->audioFrame->nb_samples,
                        m->audioEncCtx->sample_rate,
                        m->audioDecCtx->sample_rate,
                        AV_ROUND_UP);
                    av_frame_unref(m->audioEncFrame);
                    m->audioEncFrame->nb_samples = dstSamples;
                    av_frame_get_buffer(m->audioEncFrame, 0);
                    swr_convert_frame(m->audioSwr, m->audioEncFrame, m->audioFrame);
                    avcodec_send_frame(m->audioEncCtx, m->audioEncFrame);
                    while (avcodec_receive_packet(m->audioEncCtx, pkt) >= 0) {
                        pkt->stream_index = m->audioStream->index;
                        av_interleaved_write_frame(m->fmtCtx, pkt);
                        av_packet_unref(pkt);
                    }
                }
            }

            avcodec_send_packet(m->audioDecCtx, NULL);
            while (avcodec_receive_frame(m->audioDecCtx, m->audioFrame) >= 0) {
                int dstSamples = av_rescale_rnd(
                    swr_get_delay(m->audioSwr, m->audioDecCtx->sample_rate) +
                        m->audioFrame->nb_samples,
                    m->audioEncCtx->sample_rate,
                    m->audioDecCtx->sample_rate,
                    AV_ROUND_UP);
                av_frame_unref(m->audioEncFrame);
                m->audioEncFrame->nb_samples = dstSamples;
                av_frame_get_buffer(m->audioEncFrame, 0);
                swr_convert_frame(m->audioSwr, m->audioEncFrame, m->audioFrame);
                avcodec_send_frame(m->audioEncCtx, m->audioEncFrame);
                while (avcodec_receive_packet(m->audioEncCtx, pkt) >= 0) {
                    pkt->stream_index = m->audioStream->index;
                    av_interleaved_write_frame(m->fmtCtx, pkt);
                    av_packet_unref(pkt);
                }
            }

            avcodec_send_frame(m->audioEncCtx, NULL);
            while (avcodec_receive_packet(m->audioEncCtx, pkt) >= 0) {
                pkt->stream_index = m->audioStream->index;
                av_interleaved_write_frame(m->fmtCtx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        } else {
            for (AVPacket* apkt : *m->audioPackets) {
                if (apkt) {
                    apkt->stream_index = m->audioStream->index;
                    av_interleaved_write_frame(m->fmtCtx, apkt);
                }
            }
        }
    }

    if (m->fmtCtx && m->fmtCtx->pb)
        av_write_trailer(m->fmtCtx);

    if (m->fmtCtx && m->fmtCtx->pb && !(m->fmtCtx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m->fmtCtx->pb);

    if (m->frame) { av_frame_free(&m->frame); m->frame = nullptr; }
    if (m->encCtx) { avcodec_free_context(&m->encCtx); m->encCtx = nullptr; }
    if (m->fmtCtx) { avformat_free_context(m->fmtCtx); m->fmtCtx = nullptr; }

    if (m->audioDecCtx)   { avcodec_free_context(&m->audioDecCtx); m->audioDecCtx = nullptr; }
    if (m->audioEncCtx)   { avcodec_free_context(&m->audioEncCtx); m->audioEncCtx = nullptr; }
    if (m->audioFrame)    { av_frame_free(&m->audioFrame); m->audioFrame = nullptr; }
    if (m->audioEncFrame) { av_frame_free(&m->audioEncFrame); m->audioEncFrame = nullptr; }
    if (m->audioSwr)      { swr_free(&m->audioSwr); m->audioSwr = nullptr; }
    if (m->swsCtx)        { sws_freeContext(m->swsCtx); m->swsCtx = nullptr; }

    // Cleanup CUDA hwcontext (NVENC zero-copy)
    if (m->cudaFrame)         { av_frame_free(&m->cudaFrame); m->cudaFrame = nullptr; }
    if (m->hwFramesCtx)       { av_buffer_unref(&m->hwFramesCtx); m->hwFramesCtx = nullptr; }
    if (m->hwDeviceCtx)       { av_buffer_unref(&m->hwDeviceCtx); m->hwDeviceCtx = nullptr; }
    m->useCUDA = false;

    m->frameCount = 0;
    m->encCtxFailed = false;
    m->videoStream = nullptr;
    m->audioStream = nullptr;
    m->audioPackets = nullptr;
    m->audioTranscoding = false;
}
