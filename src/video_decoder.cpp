#include "video_decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <vector>

struct VideoDecoder::Impl {
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext*  decCtx  = nullptr;
    AVFrame*         decoded = nullptr;
    SwsContext*      swsCtx  = nullptr;

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

bool VideoDecoder::Open(const wchar_t* path, VideoInfo* info) {
    Close();

    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, NULL, NULL);

    if (avformat_open_input(&m->fmtCtx, pathA, NULL, NULL) < 0)
        return false;
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
    if (avcodec_open2(m->decCtx, codec, NULL) < 0) return false;

    m->decoded = av_frame_alloc();
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

    // Find audio stream
    m->audioStreamIdx = av_find_best_stream(m->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, m->videoStreamIdx, NULL, 0);
    if (m->audioStreamIdx >= 0) {
        AVStream* audioStream = m->fmtCtx->streams[m->audioStreamIdx];
        info->audioStreamIndex = m->audioStreamIdx;
        info->audioSampleRate = audioStream->codecpar->sample_rate;
        info->audioChannels = audioStream->codecpar->ch_layout.nb_channels;
        info->hasAudio = true;

        // Copy codecpar for encoder remux
        m->audioCodecPar = avcodec_parameters_alloc();
        if (m->audioCodecPar) {
            avcodec_parameters_copy(m->audioCodecPar, audioStream->codecpar);
        }
    }

    return true;
}

bool VideoDecoder::ReadFrameNV12(uint8_t* outData, int* outStride) {
    if (!m->fmtCtx) return false;

    AVPacket pkt;
    while (av_read_frame(m->fmtCtx, &pkt) >= 0) {
        int si = pkt.stream_index;
        if (si == m->videoStreamIdx) {
            if (avcodec_send_packet(m->decCtx, &pkt) == 0) {
                int ret = avcodec_receive_frame(m->decCtx, m->decoded);
                av_packet_unref(&pkt);
                if (ret == 0) {
                    // Convert to NV12 via swscale
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
        } else if (m->audioPackets && si == m->audioStreamIdx) {
            AVPacket* copy = av_packet_alloc();
            av_packet_ref(copy, &pkt);
            m->audioPackets->push_back(copy);
            av_packet_unref(&pkt);
        } else {
            av_packet_unref(&pkt);
        }
    }
    return false;  // EOF or error
}

void VideoDecoder::Close() {
    if (m->swsCtx) sws_freeContext(m->swsCtx);
    if (m->decoded) av_frame_free(&m->decoded);
    if (m->decCtx) avcodec_free_context(&m->decCtx);
    if (m->fmtCtx) avformat_close_input(&m->fmtCtx);
    if (m->audioCodecPar) { avcodec_parameters_free(&m->audioCodecPar); m->audioCodecPar = nullptr; }
    m->videoStreamIdx = -1;
    m->audioStreamIdx = -1;
    m->swsCtx = nullptr;
}

void VideoDecoder::SetAudioPacketQueue(void* queue) {
    m->audioPackets = static_cast<std::vector<AVPacket*>*>(queue);
}

void* VideoDecoder::GetAudioCodecPar() const {
    return m->audioCodecPar;
}
