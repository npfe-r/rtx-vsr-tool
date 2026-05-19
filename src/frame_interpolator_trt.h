#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <cuda.h>
#include <cuda_runtime.h>

// ── RIFE TensorRT 帧插值引擎 ──
// 使用 TensorRT C++ API 加载预转换的 RIFE ONNX 模型。
// 输入/输出均为 RGBA CUdeviceptr，与现有管线无缝对接。
//
// RIFE ONNX 约定（vs-mlrt v4.6 格式）:
//   单输入: "input"  (1, 6, H, W) float — prev||curr RGB 拼接
//   单输出: "output" (1, 3, H, W) float — 插值帧 RGB

class FrameInterpolatorRIFE {
public:
    FrameInterpolatorRIFE();
    ~FrameInterpolatorRIFE();

    // 检查 ONNX 模型文件是否存在
    static bool IsModelPresent(const char* modelPath);

    // 初始化: 构建/加载 TensorRT engine + 分配 GPU 缓冲区
    // width, height: 工作分辨率
    // gpuIndex: CUDA 设备序号
    // onnxModelPath: ONNX 文件路径（绝对或相对 exe）
    bool Initialize(int width, int height, int gpuIndex, const char* onnxModelPath);

    // 逐帧处理。每帧都要调用，首帧无输出。
    // rgbaSrc: 当前帧 RGBA (CUdeviceptr)
    // timestamp: 时间戳（秒），单调递增
    // outRGBA: [out] 插值帧 RGBA (CUdeviceptr)
    // outFrameRepeat: [out] 场景切换标志
    // return: true=有输出, false=首帧/错误
    bool ProcessFrame(uint64_t rgbaSrc, double timestamp,
                      uint64_t& outRGBA, bool& outFrameRepeat);

    // 2x 插帧: inputFrames * 2 - 1
    int  GetExpectedOutputFrames(int inputFrames) const;

    // 释放所有资源
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    std::function<void(const char*)> onLog;

private:
    bool LoadOrBuildEngine(const char* onnxPath);
    void DestroyEngine();

    // TensorRT opaque handles
    void* m_trtRuntime = nullptr;   // nvinfer1::IRuntime*
    void* m_trtEngine  = nullptr;   // nvinfer1::ICudaEngine*
    void* m_trtContext = nullptr;   // nvinfer1::IExecutionContext*

    // ONNX tensor 名称（自动检测）
    std::string m_inputName;
    std::string m_outputName;

    // GPU 缓冲区
    uint64_t m_prevRgba    = 0;     // 上一帧 RGBA (byte)
    uint64_t m_rgbFloat6ch = 0;     // 6ch float tensor (prev||curr)
    uint64_t m_rgbFloatOut = 0;     // 3ch float output (插值帧)
    uint64_t m_rgbaOutput  = 0;     // 插值帧 RGBA (byte)

    bool m_hasPrev = false;         // 是否有上一帧缓存
    int  m_width  = 0;
    int  m_height = 0;
    int  m_alignedW = 0;            // 对齐到 32 的宽度 (RIFE 要求)
    int  m_alignedH = 0;            // 对齐到 32 的高度
    int  m_gpuIndex = 0;
    bool m_initialized = false;
    bool m_engineReady = false;

    CUcontext m_cuContext = nullptr;
    CUdevice  m_cuDevice  = 0;
};
