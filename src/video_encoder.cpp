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

#include "debug_util.h"

// Map AVColorSpace to libswscale SWS_CS_* constant for sws_setColorspaceDetails.
static int AvColorSpaceToSWS(int avCS) {
    switch (avCS) {
        case 5:  case 6:  return 0;          // SWS_CS_ITU601
        case 1:           return 1;          // SWS_CS_ITU709
        case 9:  case 10: return 9;          // SWS_CS_BT2020
        default:          return 1;          // default BT.709
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

    // Status callback (stored from Open() for use in Close() audio processing)
    OnEncoderStatus statusCb = nullptr;

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
        case 6:  return "libsvtav1";
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
    if (strcmp(name, "libsvtav1") == 0) return "SVT-AV1";
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
    m->statusCb = statusCb;

    { char _b[256]; snprintf(_b, sizeof(_b), "Open encoder: codecId=%d width=%d height=%d fps=%.1f crf=%d speed=%d container=%d",
        cfg.codecId, cfg.width, cfg.height, cfg.fps, cfg.crf, cfg.speed, cfg.container); LogMsg("ENC: ",_b); }

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

    // 10-bit encoder validation (H.264 NVENC and libx264 do not support 10-bit)
    if (cfg.use10Bit && (cfg.codecId == 0 || cfg.codecId == 3)) {
        if (statusCb) {
            char msg[128];
            snprintf(msg, sizeof(msg), "编码器 %s 不支持 10-bit 输出 (TrueHDR)", displayName);
            statusCb(msg);
        }
        return false;
    }

    const char* ext = GetContainerExt(cfg.container);
    avformat_alloc_output_context2(&m->fmtCtx, NULL, ext, NULL);
    if (!m->fmtCtx) { LogMsg("ENC: ","avformat_alloc_output_context2 failed"); return false; }
    m->videoStream = avformat_new_stream(m->fmtCtx, NULL);

    m->encCtx = avcodec_alloc_context3(codec);
    m->encCtx->width     = cfg.width;
    m->encCtx->height    = cfg.height;
    AVRational fpsRat = av_d2q(cfg.fps, 1001);
    m->encCtx->time_base = av_inv_q(fpsRat);
    m->encCtx->framerate = fpsRat;
    if (cfg.use10Bit) {
        m->encCtx->pix_fmt = m->pixFmt = IsNVENCByName(encName) ? AV_PIX_FMT_CUDA : AV_PIX_FMT_YUV420P10LE;
    } else {
        m->encCtx->pix_fmt = m->pixFmt = IsNVENCByName(encName) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    }
    m->encCtx->color_range = AVCOL_RANGE_MPEG;
    if (cfg.use10Bit) {
        // HDR10 metadata: BT.2020 primaries, PQ transfer, BT.2020_NCL matrix.
        // When TrueHDR is active the output is always in HDR10 colour space
        // regardless of source metadata — the NGX TrueHDR engine tone-maps
        // SDR content into the HDR BT.2020 PQ container internally.
        m->encCtx->color_primaries = AVCOL_PRI_BT2020;
        m->encCtx->color_trc       = AVCOL_TRC_SMPTE2084;  // PQ / ST 2084
        m->encCtx->colorspace      = AVCOL_SPC_BT2020_NCL;
    } else {
        // Use source colour metadata when available, fall back to BT.709.
        // "Unspecified" in FFmpeg is either 0 (Reserved) or 2.
        m->encCtx->color_primaries =
            (cfg.colorPrimaries > 0 && cfg.colorPrimaries != 2)
            ? (AVColorPrimaries)cfg.colorPrimaries : AVCOL_PRI_BT709;
        m->encCtx->color_trc =
            (cfg.colorTransfer > 0 && cfg.colorTransfer != 2)
            ? (AVColorTransferCharacteristic)cfg.colorTransfer : AVCOL_TRC_BT709;
        m->encCtx->colorspace =
            (cfg.colorSpace > 0 && cfg.colorSpace != 2)
            ? (AVColorSpace)cfg.colorSpace : AVCOL_SPC_BT709;
    }
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
                hwfc->sw_format = cfg.use10Bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
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
                    LogMsg("ENC: ","av_hwframe_ctx_init failed for CUDA hwframe pool");
                    av_buffer_unref(&m->hwDeviceCtx);
                    m->hwDeviceCtx = nullptr;
                }
            } else {
                LogMsg("ENC: ","av_hwdevice_ctx_create failed for CUDA");
            }
        }
    } else {
        char crfStr[8];
        snprintf(crfStr, sizeof(crfStr), "%d", cfg.crf);
        av_opt_set(m->encCtx->priv_data, "crf", crfStr, 0);

        // Don't set thread_count explicitly for software encoders — leave at 0
        // (auto) so the encoder manages its own thread pool.  Setting it to
        // hardware_concurrency() can cause x265_encoder_open to reject the
        // configuration with AVERROR_INVALIDDATA if the pool size string
        // format is incompatible with the x265 version in the FFmpeg build.

        // Codec-specific options: each software encoder uses different
        // private AVOptions.  Applying libaom-av1-specific options
        // (cpu-used, usage, row-mt, lag-in-frames, tile-*) to other
        // encoders may cause x265_encoder_open to fail with AVERROR_INVALIDDATA.
        const char* swPresets[] = { "ultrafast", "superfast", "veryfast", "medium", "veryslow" };
        int idx = cfg.speed;
        if (idx < 0) idx = 0;
        if (idx > 4) idx = 4;

        if (strcmp(encName, "libx264") == 0) {
            av_opt_set(m->encCtx->priv_data, "preset", swPresets[idx], 0);
        } else if (strcmp(encName, "libx265") == 0) {
            av_opt_set(m->encCtx->priv_data, "preset", swPresets[idx], 0);
        } else if (strcmp(encName, "libaom-av1") == 0) {
            // libaom maps avctx->thread_count directly to its internal
            // g_threads (tile thread pool).  Default is 1 from
            // avcodec_alloc_context3, which limits tile encoding to a
            // single thread regardless of tile-count or row-mt settings.
            // Setting to 0 lets the encoder's init function use av_cpu_count().
            m->encCtx->thread_count = 0;
            int cpuUsed = 0;
            const char* usage = "good";
            switch (idx) {
                case 0: cpuUsed = 6; usage = "realtime"; break;
                case 1: cpuUsed = 5; usage = "realtime"; break;
                case 2: cpuUsed = 3; usage = "good";     break;
                case 3: cpuUsed = 1; usage = "good";     break;
                case 4: cpuUsed = 0; usage = "good";     break;
            }
            av_opt_set_int(m->encCtx->priv_data, "cpu-used", cpuUsed, 0);
            av_opt_set(m->encCtx->priv_data, "usage", usage, 0);
            av_opt_set_int(m->encCtx->priv_data, "row-mt", 1, 0);
            av_opt_set_int(m->encCtx->priv_data, "lag-in-frames", 0, 0);
            av_opt_set_int(m->encCtx->priv_data, "arnr-maxframes", 0, 0);
            av_opt_set_int(m->encCtx->priv_data, "arnr-strength", 0, 0);
            // Tile parallelism: tile-columns/rows = log2 of tile count per dimension.
            // Aggressive tiling keeps CPU cores fed — each tile is independent.
            // Min tile size ~320px well above libaom's 64px superblock minimum.
            int tc = 0, tr = 0;
            if (cfg.width >= 1920)       tc = 2;  // 4 column tiles
            else if (cfg.width >= 1280)  tc = 1;  // 2 column tiles
            if (cfg.height >= 1080)      tr = 1;  // 2 row tiles (4K → 8 tiles, 1080p → 4 tiles)
            av_opt_set_int(m->encCtx->priv_data, "tile-columns", tc, 0);
            av_opt_set_int(m->encCtx->priv_data, "tile-rows", tr, 0);
        } else if (strcmp(encName, "libsvtav1") == 0) {
            int svtPreset = 8;
            switch (idx) {
                case 0: svtPreset = 12; break;
                case 1: svtPreset = 10; break;
                case 2: svtPreset = 8;  break;
                case 3: svtPreset = 4;  break;
                case 4: svtPreset = 2;  break;
            }
            av_opt_set_int(m->encCtx->priv_data, "preset", svtPreset, 0);
        } else {
            LogMsg("ENC: ","Unknown software encoder, setting only CRF");
        }
    }

    int ret = avcodec_open2(m->encCtx, codec, NULL);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        { char _b[384]; snprintf(_b, sizeof(_b), "avcodec_open2 failed: ret=%d (%s)", ret, err); LogMsg("ENC: ",_b); }
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

    // Initialize swscale for NV12/P010 → encoder pixel format (software encoders).
    // Preserve the source colour matrix so the conversion yields correct pixel values.
    {
        int encColorspace = AvColorSpaceToSWS(cfg.colorSpace);
        int encSrcRange = 0;  // pipeline output is always limited range
        if (m->pixFmt == AV_PIX_FMT_YUV420P10LE) {
            m->swsCtx = sws_getContext(
                cfg.width, cfg.height, AV_PIX_FMT_P010LE,
                cfg.width, cfg.height, AV_PIX_FMT_YUV420P10LE,
                SWS_BILINEAR, NULL, NULL, NULL);
            { char _b[256]; snprintf(_b, sizeof(_b), "sws: P010LE → YUV420P10LE (%dx%d) cs=%d",
                cfg.width, cfg.height, encColorspace); LogMsg("ENC: ",_b); }
        } else if (m->pixFmt == AV_PIX_FMT_YUV420P) {
            m->swsCtx = sws_getContext(
                cfg.width, cfg.height, AV_PIX_FMT_NV12,
                cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, NULL, NULL, NULL);
        }
        if (m->swsCtx) {
            sws_setColorspaceDetails(m->swsCtx,
                sws_getCoefficients(encColorspace), encSrcRange,
                sws_getCoefficients(encColorspace), 0,
                0, 1 << 16, 1 << 16);
        }
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
        { char _b[512]; snprintf(_b, sizeof(_b), "avio_open(%s)...", pathA); LogMsg("ENC: ",_b); }
        if (avio_open(&m->fmtCtx->pb, pathA, AVIO_FLAG_WRITE) < 0) {
            LogMsg("ENC: ","avio_open failed");
            return false;
        }
        LogMsg("ENC: ","avio_open OK");
    }

    avformat_write_header(m->fmtCtx, NULL);

    // Allocate CPU encoding frame (CUDA path uses m->cudaFrame from hwframe pool instead)
    if (!m->useCUDA) {
        m->frame = av_frame_alloc();
        m->frame->width  = cfg.width;
        m->frame->height = cfg.height;
        m->frame->format = m->pixFmt;
        if (av_frame_get_buffer(m->frame, 0) < 0) {
            LogMsg("ENC: ","av_frame_get_buffer failed");
            return false;
        }
    }

    return true;
}

bool VideoEncoder::WriteFrameNV12(const uint8_t* data, int yStride, int uvStride, int64_t pts) {
    if (!m->frame || !m->encCtx) return false;

    if (m->pixFmt == AV_PIX_FMT_YUV420P10LE && m->swsCtx) {
        // P010 → YUV420P10LE (10-bit software encoders)
        const uint8_t* srcData[2] = {
            data,
            data + yStride * m->encCtx->height
        };
        int srcLinesizes[2] = { yStride, uvStride };
        sws_scale(m->swsCtx, srcData, srcLinesizes, 0, m->encCtx->height,
                  m->frame->data, m->frame->linesize);
    } else if (m->pixFmt == AV_PIX_FMT_YUV420P && m->swsCtx) {
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
        LogMsg("ENC: ","SEH in avcodec_send_frame");
        m->encCtxFailed = true;
        return false;
    }

    AVPacket pkt = { 0 };
    while (true) {
        __try {
            int ret = avcodec_receive_packet(m->encCtx, &pkt);
            if (ret < 0) break;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("ENC: ","SEH in avcodec_receive_packet");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        __try {
            av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
            pkt.stream_index = m->videoStream->index;
            av_interleaved_write_frame(m->fmtCtx, &pkt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("ENC: ","SEH in av_interleaved_write_frame");
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
        LogMsg("ENC: ","GetFrameBuffer: av_hwframe_get_buffer failed");
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
            char _b[256]; snprintf(_b, sizeof(_b), "SubmitFrame: avcodec_send_frame failed: %s", _e); LogMsg("ENC: ",_b);
            m->encCtxFailed = true;
            av_frame_unref(m->cudaFrame);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMsg("ENC: ","SEH in avcodec_send_frame (CUDA)");
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
            LogMsg("ENC: ","SEH in avcodec_receive_packet (CUDA)");
            m->encCtxFailed = true;
            av_packet_unref(&pkt);
            return false;
        }
        __try {
            av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
            pkt.stream_index = m->videoStream->index;
            av_interleaved_write_frame(m->fmtCtx, &pkt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("ENC: ","SEH in av_interleaved_write_frame (CUDA)");
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
            LogMsg("ENC: ","SEH in Close flush send_frame");
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
            LogMsg("ENC: ","SEH in Close flush receive_packet");
        }
    }

    // Audio (transcode or remux) - only if fmtCtx was fully opened
    if (m->fmtCtx && m->fmtCtx->pb && m->audioStream && m->audioPackets) {
        if (m->audioTranscoding) {
            if (m->statusCb) m->statusCb("音频转码中...");

            // Allocate audio frame buffers once (max possible size)
            int maxSamples = 2048; // safe upper bound for AAC frame size
            m->audioEncFrame->format      = m->audioEncCtx->sample_fmt;
            m->audioEncFrame->ch_layout   = m->audioEncCtx->ch_layout;
            m->audioEncFrame->sample_rate = m->audioEncCtx->sample_rate;
            m->audioEncFrame->nb_samples  = maxSamples;
            if (av_frame_get_buffer(m->audioEncFrame, 0) < 0) {
                LogMsg("ENC: ","audio: failed to allocate encoder frame buffer");
            }

            // Lambda: decode one packet → resample → encode → write
            auto processAudioPacket = [&](AVPacket* apkt) -> bool {
                if (!apkt) return false;
                if (avcodec_send_packet(m->audioDecCtx, apkt) < 0)
                    return false;
                while (avcodec_receive_frame(m->audioDecCtx, m->audioFrame) >= 0) {
                    int dstSamples = av_rescale_rnd(
                        swr_get_delay(m->audioSwr, m->audioDecCtx->sample_rate) +
                            m->audioFrame->nb_samples,
                        m->audioEncCtx->sample_rate,
                        m->audioDecCtx->sample_rate,
                        AV_ROUND_UP);
                    av_frame_make_writable(m->audioEncFrame);
                    if (dstSamples > m->audioEncFrame->nb_samples) {
                        // Grow buffer if needed (should not happen with 2048 initial)
                        av_frame_unref(m->audioEncFrame);
                        m->audioEncFrame->format      = m->audioEncCtx->sample_fmt;
                        m->audioEncFrame->ch_layout   = m->audioEncCtx->ch_layout;
                        m->audioEncFrame->sample_rate = m->audioEncCtx->sample_rate;
                        m->audioEncFrame->nb_samples  = dstSamples;
                        av_frame_get_buffer(m->audioEncFrame, 0);
                    }
                    m->audioEncFrame->nb_samples = dstSamples;
                    swr_convert_frame(m->audioSwr, m->audioEncFrame, m->audioFrame);
                    avcodec_send_frame(m->audioEncCtx, m->audioEncFrame);
                    AVPacket* opkt = av_packet_alloc();
                    while (avcodec_receive_packet(m->audioEncCtx, opkt) >= 0) {
                        opkt->stream_index = m->audioStream->index;
                        av_interleaved_write_frame(m->fmtCtx, opkt);
                        av_packet_unref(opkt);
                    }
                    av_packet_free(&opkt);
                }
                return true;
            };

            // Lambda: flush decoder (send NULL, drain remaining frames)
            auto flushDecoder = [&]() {
                avcodec_send_packet(m->audioDecCtx, NULL);
                while (avcodec_receive_frame(m->audioDecCtx, m->audioFrame) >= 0) {
                    int dstSamples = av_rescale_rnd(
                        swr_get_delay(m->audioSwr, m->audioDecCtx->sample_rate) +
                            m->audioFrame->nb_samples,
                        m->audioEncCtx->sample_rate,
                        m->audioDecCtx->sample_rate,
                        AV_ROUND_UP);
                    av_frame_make_writable(m->audioEncFrame);
                    if (dstSamples > m->audioEncFrame->nb_samples) {
                        av_frame_unref(m->audioEncFrame);
                        m->audioEncFrame->format      = m->audioEncCtx->sample_fmt;
                        m->audioEncFrame->ch_layout   = m->audioEncCtx->ch_layout;
                        m->audioEncFrame->sample_rate = m->audioEncCtx->sample_rate;
                        m->audioEncFrame->nb_samples  = dstSamples;
                        av_frame_get_buffer(m->audioEncFrame, 0);
                    }
                    m->audioEncFrame->nb_samples = dstSamples;
                    swr_convert_frame(m->audioSwr, m->audioEncFrame, m->audioFrame);
                    avcodec_send_frame(m->audioEncCtx, m->audioEncFrame);
                    AVPacket* opkt = av_packet_alloc();
                    while (avcodec_receive_packet(m->audioEncCtx, opkt) >= 0) {
                        opkt->stream_index = m->audioStream->index;
                        av_interleaved_write_frame(m->fmtCtx, opkt);
                        av_packet_unref(opkt);
                    }
                    av_packet_free(&opkt);
                }
            };

            // Process all source packets
            int totalPkts = (int)m->audioPackets->size();
            int pktCount = 0, nextReportPct = 25;
            for (AVPacket* apkt : *m->audioPackets) {
                if (!apkt) continue;
                pktCount++;
                int pct = pktCount * 100 / totalPkts;
                if (pct >= nextReportPct && m->statusCb) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "音频转码中... %d%%", pct);
                    m->statusCb(msg);
                    nextReportPct = pct + 25;
                }
                processAudioPacket(apkt);
            }

            // Flush decoder, then encoder
            flushDecoder();
            avcodec_send_frame(m->audioEncCtx, NULL);
            AVPacket* fpkt = av_packet_alloc();
            while (avcodec_receive_packet(m->audioEncCtx, fpkt) >= 0) {
                fpkt->stream_index = m->audioStream->index;
                av_interleaved_write_frame(m->fmtCtx, fpkt);
                av_packet_unref(fpkt);
            }
            av_packet_free(&fpkt);

            // Free the encoder frame buffer that we allocated once
            if (m->audioEncFrame) av_frame_unref(m->audioEncFrame);
        } else {
            if (m->statusCb) m->statusCb("音频复制中...");
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
    m->statusCb = nullptr;
    m->audioTranscoding = false;
}
