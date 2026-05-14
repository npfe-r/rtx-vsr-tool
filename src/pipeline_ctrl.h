#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <cuda_runtime.h>

#include "video_decoder.h"
#include "vsr_processor.h"
#include "video_encoder.h"

// Forward declare CUDA kernel wrappers (from cuda_yuv.cu, extern "C")
extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream);

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream);

struct PipelineConfig {
    std::wstring inputPath;
    std::wstring outputPath;
    int qualityLevel = 3;       // 0-4
    int outputMode   = 0;       // 0=2x, 1=4x, 2=fixed
    int outputWidth  = 0;
    int outputHeight = 0;
    int encoderIndex = 0;
    int crf = 18;
    int encoderSpeed = 2;
    int gpuIndex = 0;
    int container = 0;
    int outputFps = 0;          // 0 = source fps
};

struct PipelineProgress {
    int currentFrame = 0;
    int totalFrames  = 0;
    float fps = 0.0f;
    float avgMsPerFrame = 0.0f;
    float etaSeconds = 0.0f;
};

enum class PipelineState {
    Idle, Starting, Running, Paused, Completed, Error
};

class PipelineController {
public:
    PipelineController();
    ~PipelineController();

    bool Start(const PipelineConfig& cfg);
    void Pause();
    void Resume();
    void Stop();
    PipelineState GetState() const { return m_state.load(); }

    std::function<void(const PipelineProgress&)> onProgress;
    std::function<void(const wchar_t* msg)> onError;
    std::function<void()> onCompleted;

private:
    void ThreadFunc();
    void ThreadFuncImpl();
    void CalculateOutputSize(int srcW, int srcH, int& dstW, int& dstH) const;

    static const int NUM_SLOTS = 3;
    struct FrameSlot {
        // CPU buffers
        uint8_t* nv12_cpu     = nullptr;  // source size, decoder output
        uint8_t* nv12_out_cpu = nullptr;  // dest size, encoder input
        // GPU buffers
        uint8_t* d_nv12     = nullptr;    // source NV12 on GPU
        uint8_t* d_rgba_src = nullptr;    // source RGBA (YUV->RGB output -> VSR input)
        uint8_t* d_rgba_dst = nullptr;    // dest RGBA (VSR output -> RGB->YUV input)
        uint8_t* d_nv12_out = nullptr;    // dest NV12 on GPU

        int w = 0, h = 0;
        int dstW = 0, dstH = 0;
    };

    FrameSlot m_slots[NUM_SLOTS];
    int m_srcW = 0, m_srcH = 0;
    int m_dstW = 0, m_dstH = 0;
    double m_srcFps = 0.0;
    int m_totalFrames = 0;

    VideoDecoder   m_decoder;
    VSRProcessor   m_vsr;
    VideoEncoder   m_encoder;

    // Audio packet queue (shared between decoder and encoder)
    std::vector<void*> m_audioPackets;  // AVPacket*

    std::atomic<PipelineState> m_state{PipelineState::Idle};
    std::thread m_thread;
    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    PipelineConfig m_cfg;
};
