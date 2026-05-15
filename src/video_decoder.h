#pragma once
#include <windows.h>
#include <cstdint>

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int totalFrames = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    char videoCodecName[32] = "";
    char audioCodecName[32] = "";

    // Audio info for remux
    int audioStreamIndex = -1;
    int audioSampleRate = 0;
    int audioChannels = 0;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool Open(const wchar_t* path, VideoInfo* info);
    bool ReadFrameNV12(uint8_t* outData, int* outStride);  // out: stride(y), stride(uv)
    void Close();
    bool IsOpen() const;

    // Pass a pointer to a std::vector<AVPacket*> for audio packet storage
    void SetAudioPacketQueue(void* queue);

    // Returns AVCodecParameters* of the audio stream (caller must NOT free)
    void* GetAudioCodecPar() const;

private:
    struct Impl;
    Impl* m;
    bool m_eof = false;
    bool m_drainSent = false;
};
