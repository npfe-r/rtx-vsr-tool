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

    // Source PTS timebase (from video stream)
    int srcTimeBaseNum = 1;
    int srcTimeBaseDen = 30;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool Open(const wchar_t* path, VideoInfo* info, bool useGPU = false);
    bool ReadFrameNV12(uint8_t* outData, int* outStride);  // out: stride(y), stride(uv)

    // GPU decode: get device pointers directly (no CPU copy)
    // Y/UV pointers are GPU device pointers valid until next ReadFrameGPU call
    // PTS reorder buffer: the decoder maintains a multi-frame buffer and returns
    // frames in display order (sorted by PTS) — the returned outPTS value reflects
    // the presentation timestamp from the container.
    bool ReadFrameGPU(const uint8_t** outY, int* yPitch,
                      const uint8_t** outUV, int* uvPitch,
                      int64_t* outPTS = nullptr);
    bool IsHWDecoding() const;
    void Close();
    bool IsOpen() const;

    // Pass a pointer to a std::vector<AVPacket*> for audio packet storage
    void SetAudioPacketQueue(void* queue);

    // Returns PTS of the last decoded frame, or -1 if unavailable
    int64_t GetLastPTS() const;

    // Returns AVCodecParameters* of the audio stream (caller must NOT free)
    void* GetAudioCodecPar() const;

private:
    struct Impl;
    Impl* m;
    bool m_eof = false;
    bool m_drainSent = false;
    bool DecodeOne();  // shared decode: returns true if m->decoded has a new frame
};
