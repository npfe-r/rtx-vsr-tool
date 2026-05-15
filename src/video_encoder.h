#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <functional>

struct EncodeConfig {
    const wchar_t* outputPath = L"";
    int width = 0;
    int height = 0;
    double fps = 30.0;

    // 0=H264_NVENC, 1=HEVC_NVENC, 2=AV1_NVENC,
    // 3=libx264, 4=libx265, 5=libaom-av1
    int codecId = 0;
    int crf = 18;
    int speed = 2;       // 0=fast...4=slow
    int container = 0;   // 0=mp4, 1=mkv, 2=mov

    // Audio remux (optional)
    bool hasAudio = false;
    int audioStreamIdx = -1;
    int audioSampleRate = 0;
    int audioChannels = 0;
    int audioMode = 2;              // 0=no audio, 1=copy source, 2=AAC encode
    int audioBitrate = 128;         // kbps (AAC)
    void* audioPackets = nullptr;     // std::vector<AVPacket*>*
    void* audioCodecPar = nullptr;    // AVCodecParameters* (copied from source)
};

using OnEncoderStatus = std::function<void(const char*)>;

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool Open(const EncodeConfig& cfg, OnEncoderStatus statusCb = nullptr);
    bool WriteFrameNV12(const uint8_t* data, int yStride, int uvStride);
    void Close();
    bool IsOpen() const;

private:
    struct Impl;
    Impl* m;
};
