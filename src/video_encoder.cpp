#include "video_encoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libswresample/swresample.h>
}

#include <cstdio>
#include <windows.h>

static void EncLog(const char* msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#ifdef _DEBUG
    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "ENC: %s\n", msg); fflush(f); fclose(f); }
#endif
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

static const char* GetContainerExt(int container) {
    switch (container) {
        case 0: return "mp4";
        case 1: return "mkv";
        case 2: return "mov";
        default: return "mp4";
    }
}

VideoEncoder::VideoEncoder() : m(new Impl) {}
VideoEncoder::~VideoEncoder() { Close(); delete m; }
bool VideoEncoder::IsOpen() const { return m->fmtCtx != nullptr; }

bool VideoEncoder::Open(const EncodeConfig& cfg) {
    Close();

    { char _b[256]; snprintf(_b, sizeof(_b), "Open encoder: codecId=%d width=%d height=%d fps=%.1f crf=%d speed=%d container=%d",
        cfg.codecId, cfg.width, cfg.height, cfg.fps, cfg.crf, cfg.speed, cfg.container); EncLog(_b); }

    const char* encoderName = GetEncoderName(cfg.codecId);
    { char _b[256]; snprintf(_b, sizeof(_b), "Looking for encoder: %s", encoderName); EncLog(_b); }

    const AVCodec* codec = avcodec_find_encoder_by_name(encoderName);
    bool isNVENC = (cfg.codecId <= 2);

    if (!codec) {
        EncLog("NVENC codec not found, trying fallback");
        if (isNVENC) {
            const char* fallback[] = { "libx264", "libx265", "libaom-av1" };
            codec = avcodec_find_encoder_by_name(fallback[cfg.codecId]);
            isNVENC = false;
        }
        if (!codec) { EncLog("No codec found at all!"); return false; }
        { char _b[256]; snprintf(_b, sizeof(_b), "Fallback to %s", avcodec_get_name(codec->id)); EncLog(_b); }
    } else {
        { char _b[256]; snprintf(_b, sizeof(_b), "Found encoder %s", avcodec_get_name(codec->id)); EncLog(_b); }
    }

    const char* ext = GetContainerExt(cfg.container);
    { char _b[256]; snprintf(_b, sizeof(_b), "Alloc output context, format=%s", ext); EncLog(_b); }
    avformat_alloc_output_context2(&m->fmtCtx, NULL, ext, NULL);
    if (!m->fmtCtx) { EncLog("avformat_alloc_output_context2 failed"); return false; }
    EncLog("Output context allocated");

    // Video stream
    m->videoStream = avformat_new_stream(m->fmtCtx, NULL);
    m->encCtx = avcodec_alloc_context3(codec);
    m->encCtx->width     = cfg.width;
    m->encCtx->height    = cfg.height;
    AVRational fpsRat = av_d2q(cfg.fps, 1001);
    m->encCtx->time_base = av_inv_q(fpsRat);
    m->encCtx->framerate = fpsRat;
    m->encCtx->pix_fmt   = AV_PIX_FMT_NV12;

    // NVENC settings
    if (isNVENC) {
        av_opt_set(m->encCtx->priv_data, "preset",
                   cfg.speed <= 1 ? "p1" : cfg.speed == 2 ? "p4" : "p7", 0);
        av_opt_set_int(m->encCtx->priv_data, "cq", cfg.crf, 0);
        av_opt_set(m->encCtx->priv_data, "rc", "vbr", 0);
        av_opt_set_int(m->encCtx->priv_data, "b", 0, 0);
    }
    // Software encoder settings
    else {
        char crfStr[8];
        snprintf(crfStr, sizeof(crfStr), "%d", cfg.crf);
        av_opt_set(m->encCtx->priv_data, "crf", crfStr, 0);
        const char* presets[] = {
            "ultrafast", "superfast", "veryfast", "faster", "fast",
            "medium", "slow", "slower", "veryslow"
        };
        int idx = (cfg.speed * 2);
        if (idx < 0) idx = 0;
        if (idx > 8) idx = 8;
        av_opt_set(m->encCtx->priv_data, "preset", presets[idx], 0);
    }

    { char _b[256]; snprintf(_b, sizeof(_b), "avcodec_open2(%s)...", encoderName); EncLog(_b); }
    int ret = avcodec_open2(m->encCtx, codec, NULL);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        { char _b[256]; snprintf(_b, sizeof(_b), "avcodec_open2 failed: %s (%d)", err, ret); EncLog(_b); }
        return false;
    }
    EncLog("avcodec_open2 OK");
    avcodec_parameters_from_context(m->videoStream->codecpar, m->encCtx);
    m->videoStream->time_base = m->encCtx->time_base;

    // Audio stream setup (transcode to AAC for maximum compatibility)
    if (cfg.hasAudio && cfg.audioPackets && cfg.audioStreamIdx >= 0 && cfg.audioCodecPar) {
        m->audioPackets = static_cast<std::vector<AVPacket*>*>(cfg.audioPackets);
        AVCodecParameters* srcPar = static_cast<AVCodecParameters*>(cfg.audioCodecPar);

        // Create source audio decoder
        const AVCodec* decCodec = avcodec_find_decoder(srcPar->codec_id);
        if (decCodec) {
            m->audioDecCtx = avcodec_alloc_context3(decCodec);
            if (avcodec_parameters_to_context(m->audioDecCtx, srcPar) >= 0 &&
                avcodec_open2(m->audioDecCtx, decCodec, NULL) >= 0) {

                // Create AAC encoder
                const AVCodec* encCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
                if (encCodec) {
                    m->audioEncCtx = avcodec_alloc_context3(encCodec);
                    m->audioEncCtx->sample_rate = m->audioDecCtx->sample_rate;
                    m->audioEncCtx->ch_layout   = m->audioDecCtx->ch_layout;
                    m->audioEncCtx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
                    m->audioEncCtx->bit_rate    = 128000;

                    if (avcodec_open2(m->audioEncCtx, encCodec, NULL) >= 0) {
                        // Create resampler for format conversion
                        m->audioSwr = swr_alloc();
                        av_opt_set_chlayout(m->audioSwr, "in_chlayout",  &m->audioDecCtx->ch_layout, 0);
                        av_opt_set_int(m->audioSwr, "in_sample_rate",     m->audioDecCtx->sample_rate, 0);
                        av_opt_set_sample_fmt(m->audioSwr, "in_sample_fmt", m->audioDecCtx->sample_fmt, 0);
                        av_opt_set_chlayout(m->audioSwr, "out_chlayout", &m->audioEncCtx->ch_layout, 0);
                        av_opt_set_int(m->audioSwr, "out_sample_rate",    m->audioEncCtx->sample_rate, 0);
                        av_opt_set_sample_fmt(m->audioSwr, "out_sample_fmt", m->audioEncCtx->sample_fmt, 0);
                        if (m->audioSwr && swr_init(m->audioSwr) >= 0) {
                            m->audioFrame = av_frame_alloc();
                            m->audioEncFrame = av_frame_alloc();
                            m->audioEncFrame->format     = m->audioEncCtx->sample_fmt;
                            m->audioEncFrame->ch_layout  = m->audioEncCtx->ch_layout;
                            m->audioEncFrame->sample_rate = m->audioEncCtx->sample_rate;

                            // Create audio output stream
                            m->audioStream = avformat_new_stream(m->fmtCtx, NULL);
                            avcodec_parameters_from_context(m->audioStream->codecpar, m->audioEncCtx);
                            AVRational audio_tb = { 1, m->audioEncCtx->sample_rate };
                            m->audioStream->time_base = audio_tb;
                            m->audioTranscoding = true;
                        }
                    }
                }
            }
        }

        // Fallback: remux (if transcode setup failed, try direct remux)
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

    // Allocate frame for NV12 encoding
    m->frame = av_frame_alloc();
    m->frame->width  = cfg.width;
    m->frame->height = cfg.height;
    m->frame->format = AV_PIX_FMT_NV12;
    av_frame_get_buffer(m->frame, 0);

    return true;
}

bool VideoEncoder::WriteFrameNV12(const uint8_t* data, int yStride, int uvStride) {
    if (!m->frame || !m->encCtx) return false;

    // Copy Y plane line by line (handles pitch != width)
    uint8_t* yDst = m->frame->data[0];
    int yLineSize = m->encCtx->width;
    for (int y = 0; y < m->encCtx->height; y++)
        memcpy(yDst + y * m->frame->linesize[0], data + y * yStride, yLineSize);

    // Copy UV plane
    uint8_t* uvDst = m->frame->data[1];
    int uvLineSize = m->encCtx->width;
    int uvHeight = m->encCtx->height / 2;
    for (int y = 0; y < uvHeight; y++)
        memcpy(uvDst + y * m->frame->linesize[1],
               data + yStride * m->encCtx->height + y * uvStride, uvLineSize);

    m->frame->pts = m->frameCount++;

    if (avcodec_send_frame(m->encCtx, m->frame) < 0)
        return false;

    AVPacket pkt = { 0 };
    int ret = avcodec_receive_packet(m->encCtx, &pkt);
    if (ret >= 0) {
        av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
        pkt.stream_index = m->videoStream->index;
        av_interleaved_write_frame(m->fmtCtx, &pkt);
        av_packet_unref(&pkt);
    }
    return true;
}

void VideoEncoder::Close() {
    if (m->encCtx && m->videoStream) {
        avcodec_send_frame(m->encCtx, NULL);
        AVPacket pkt = { 0 };
        while (avcodec_receive_packet(m->encCtx, &pkt) >= 0) {
            av_packet_rescale_ts(&pkt, m->encCtx->time_base, m->videoStream->time_base);
            pkt.stream_index = m->videoStream->index;
            av_interleaved_write_frame(m->fmtCtx, &pkt);
            av_packet_unref(&pkt);
        }
    }

    // Audio (transcode or remux)
    if (m->audioStream && m->audioPackets) {
        if (m->audioTranscoding) {
            // Decode source audio → encode AAC → write
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

            // Flush decoder
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

            // Flush encoder
            avcodec_send_frame(m->audioEncCtx, NULL);
            while (avcodec_receive_packet(m->audioEncCtx, pkt) >= 0) {
                pkt->stream_index = m->audioStream->index;
                av_interleaved_write_frame(m->fmtCtx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        } else {
            // Remux (fallback for compatible codecs)
            for (AVPacket* apkt : *m->audioPackets) {
                if (apkt) {
                    apkt->stream_index = m->audioStream->index;
                    av_interleaved_write_frame(m->fmtCtx, apkt);
                }
            }
        }
    }

    if (m->fmtCtx)
        av_write_trailer(m->fmtCtx);

    if (m->fmtCtx && m->fmtCtx->pb && !(m->fmtCtx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m->fmtCtx->pb);

    if (m->frame) av_frame_free(&m->frame);
    if (m->encCtx) avcodec_free_context(&m->encCtx);
    if (m->fmtCtx) avformat_free_context(m->fmtCtx);

    // Cleanup audio transcode
    if (m->audioDecCtx)   avcodec_free_context(&m->audioDecCtx);
    if (m->audioEncCtx)   avcodec_free_context(&m->audioEncCtx);
    if (m->audioFrame)    av_frame_free(&m->audioFrame);
    if (m->audioEncFrame) av_frame_free(&m->audioEncFrame);
    if (m->audioSwr)      swr_free(&m->audioSwr);

    m->frameCount = 0;
    m->videoStream = nullptr;
    m->audioStream = nullptr;
    m->audioPackets = nullptr;
    m->audioTranscoding = false;
}
