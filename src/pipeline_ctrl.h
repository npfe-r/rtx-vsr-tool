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
#include "color_types.h"

extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix, int srcRange);

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix, int srcRange);

extern "C" void launch_abgr10_to_p010(
    const uint8_t* abgr10, int abgr10_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, bool bt2020, cudaStream_t stream);

extern "C" void launch_p010_to_rgba_sdr(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, int transfer, cudaStream_t stream);

struct PipelineConfig {
    std::wstring inputPath;
    std::wstring outputPath;
    int qualityLevel = 3;
    int outputMode   = 0;
    int outputWidth  = 0;
    int outputHeight = 0;
    int encoderIndex = 0;
    int crf = 18;
    int encoderSpeed = 3;
    int gpuIndex = 0;
    int container = 0;
    int audioMode = 1;
    int audioBitrate = 128;
    bool trueHdrEnabled = false;
    int  thdrContrast    = 100;
    int  thdrSaturation  = 100;
    int  thdrMiddleGray  = 50;
    int  thdrMaxLuminance = 1000;
};

struct PipelineProgress {
    int currentFrame = 0;
    int totalFrames  = 0;
    float fps = 0.0f;
    float avgMsPerFrame = 0.0f;
    float etaSeconds = 0.0f;
    char decodeMode[16] = {};
};

enum class PipelineState {
    Idle, Starting, Running, Paused, Completed, Error
};

enum class SlotState {
    Empty,
    Decoding,
    VSR_Ready,
    Encoding
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
    std::function<void(const char* msg)> onStatus;

private:
    void ThreadFunc();
    void ThreadFuncImpl();
    void DecodeFunc();
    void CalculateOutputSize(int srcW, int srcH, int& dstW, int& dstH) const;

    static const int NUM_SLOTS = 3;
    struct FrameSlot {
        uint8_t* nv12_cpu     = nullptr;
        uint8_t* nv12_out_cpu = nullptr;
        uint8_t* d_nv12       = nullptr;
        uint8_t* d_rgba_src   = nullptr;
        uint8_t* d_rgba_dst   = nullptr;
        uint8_t* d_nv12_out   = nullptr;

        cudaStream_t stream = nullptr;
        cudaEvent_t decodeEvent = nullptr; // NVDEC default-stream → per-slot stream sync
        cudaEvent_t vsrEvent = nullptr;    // VSR default-stream → per-slot stream sync
        std::atomic<SlotState> state{SlotState::Empty};

        std::atomic<int> seq{0}; // frame sequence number, set by decode thread
        int64_t pts = -1;        // presentation timestamp from decoder

        int w = 0, h = 0;
        int dstW = 0, dstH = 0;
    };

    FrameSlot m_slots[NUM_SLOTS];
    int m_srcW = 0, m_srcH = 0;
    int m_dstW = 0, m_dstH = 0;
    double m_srcFps = 0.0;
    int m_totalFrames = 0;
    int m_srcTimeBaseNum = 1;
    int m_srcTimeBaseDen = 30;

    VideoDecoder      m_decoder;
    VSRProcessor      m_vsr;
    VideoEncoder      m_encoder;

    std::vector<void*> m_audioPackets;

    std::atomic<PipelineState> m_state{PipelineState::Idle};
    std::thread m_thread;
    std::thread m_decodeThread;
    std::mutex m_slotMutex;
    std::condition_variable m_slotCv;
    std::atomic<bool> m_decodeDone{false};
    std::atomic<int> m_framesEncoded{0};

    std::mutex m_pauseMutex;
    std::condition_variable m_pauseCv;
    PipelineConfig m_cfg;
    char m_decodeMode[16] = {};

    // Colour metadata from source (used for CUDA kernels + encoder)
    int m_colorMatrix = COLOR_MATRIX_BT709;   // ColorMatrix enum
    int m_avColorPrimaries = 2;                // AVCOL_PRI_BT709
    int m_avColorTransfer   = 2;               // AVCOL_TRC_BT709
    int m_avColorSpace      = 1;               // AVCOL_SPC_BT709
    int m_avColorRange      = 0;               // AVCOL_RANGE_UNSPECIFIED

    // TrueHDR mode (10-bit P010 output, HDR10 metadata)
    bool m_trueHdrEnabled = false;
};
