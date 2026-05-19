#include "frame_interpolator_trt.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <vector>
#include <fstream>
#include <sstream>

#include <NvInfer.h>
#include <NvOnnxParser.h>

// CUDA kernel 声明 (cuda_rife.cu)
extern "C" void launch_rgba_to_rgb_float(
    const uint8_t* rgba, int rgba_pitch,
    float* rgb_float, int w, int h,
    int offset_channels, cudaStream_t stream);

extern "C" void launch_rgb_float_to_rgba(
    const float* rgb_float, int w, int h,
    uint8_t* rgba, int rgba_pitch, cudaStream_t stream);

#include "debug_util.h"
#define LOG(x) LogMsg("RIFE: ", x)

// TensorRT 日志
class TrtLogger : public nvinfer1::ILogger {
    void log(Severity sev, const char* msg) noexcept override {
        if (sev <= Severity::kWARNING) {
            std::string buf = "[TRT] ";
            buf += msg;
            LogMsg("RIFE: ", buf.c_str());
        }
    }
};
static TrtLogger g_trtLogger;

// ============================================================
FrameInterpolatorRIFE::FrameInterpolatorRIFE() {}
FrameInterpolatorRIFE::~FrameInterpolatorRIFE() { Shutdown(); }

// ============================================================
bool FrameInterpolatorRIFE::IsModelPresent(const char* modelPath) {
    std::ifstream f(modelPath);
    return f.good();
}

// ============================================================
bool FrameInterpolatorRIFE::Initialize(
    int width, int height, int gpuIndex, const char* onnxModelPath)
{
    Shutdown();
    m_width = width; m_height = height; m_gpuIndex = gpuIndex;

    if (!IsModelPresent(onnxModelPath)) {
        char b[512]; snprintf(b, sizeof(b), "ONNX 模型未找到: %s", onnxModelPath);
        LOG(b); return false;
    }

    // CUDA 设备
    cudaSetDevice(gpuIndex);
    cuDeviceGet(&m_cuDevice, gpuIndex);
    cuDevicePrimaryCtxRetain(&m_cuContext, m_cuDevice);
    cuCtxSetCurrent(m_cuContext);

    // TensorRT engine
    if (!LoadOrBuildEngine(onnxModelPath)) { Shutdown(); return false; }

    // 分配 GPU 内存
    size_t rgba4sz  = (size_t)width * height * 4;
    size_t rgb6ch   = (size_t)width * height * 6 * sizeof(float);
    size_t rgb3ch   = (size_t)width * height * 3 * sizeof(float);

    auto cuAlloc = [](uint64_t& p, size_t sz) -> bool {
        return (sz == 0) || (cuMemAlloc((CUdeviceptr*)&p, sz) == CUDA_SUCCESS);
    };

    if (!cuAlloc(m_prevRgba,    rgba4sz))  { LOG("alloc prevRgba failed");     Shutdown(); return false; }
    if (!cuAlloc(m_rgbFloat6ch, rgb6ch))   { LOG("alloc rgbFloat6ch failed"); Shutdown(); return false; }
    if (!cuAlloc(m_rgbFloatOut, rgb3ch))   { LOG("alloc rgbFloatOut failed"); Shutdown(); return false; }
    if (!cuAlloc(m_rgbaOutput,  rgba4sz))  { LOG("alloc rgbaOutput failed");  Shutdown(); return false; }

    m_hasPrev = false;
    m_initialized = true;
    LOG("FrameInterpolatorRIFE 初始化完成");
    return true;
}

// ============================================================
// LoadOrBuildEngine
// ============================================================
bool FrameInterpolatorRIFE::LoadOrBuildEngine(const char* onnxPath) {
    using namespace nvinfer1;
    std::string cachePath = std::string(onnxPath) + ".engine";

    // 1. 尝试加载缓存 .engine
    {
        std::ifstream f(cachePath, std::ios::binary);
        if (f) {
            LOG("加载缓存的 TensorRT engine...");
            f.seekg(0, std::ios::end);
            std::vector<uint8_t> blob((size_t)f.tellg());
            f.seekg(0); f.read((char*)blob.data(), blob.size()); f.close();

            m_trtRuntime = createInferRuntime(g_trtLogger);
            m_trtEngine  = ((IRuntime*)m_trtRuntime)->deserializeCudaEngine(blob.data(), blob.size());
            if (m_trtEngine) {
                m_trtContext = ((ICudaEngine*)m_trtEngine)->createExecutionContext();
                // 获取 I/O 名称
                m_inputName  = ((ICudaEngine*)m_trtEngine)->getIOTensorName(0);
                m_outputName = ((ICudaEngine*)m_trtEngine)->getIOTensorName(1);
                m_engineReady = true;
                LOG("缓存 engine 加载成功");
                return true;
            }
            LOG("缓存加载失败，重新构建...");
        }
    }

    // 2. 从 ONNX 构建
    LOG("从 ONNX 构建 TensorRT engine（首次约 10-30 秒）...");

    IBuilder* builder = createInferBuilder(g_trtLogger);
    if (!builder) { LOG("createInferBuilder 失败"); return false; }

    uint32_t flag = 1U << (uint32_t)NetworkDefinitionCreationFlag::kEXPLICIT_BATCH;
    INetworkDefinition* net = builder->createNetworkV2(flag);
    auto parser = nvonnxparser::createParser(*net, g_trtLogger);
    if (!parser->parseFromFile(onnxPath, 0)) {
        LOG("ONNX 解析失败"); parser->destroy(); net->destroy(); builder->destroy(); return false;
    }

    // 检测 I/O 名称
    for (int i = 0; i < net->getNbInputs(); i++)
        m_inputName = net->getInput(i)->getName();
    for (int i = 0; i < net->getNbOutputs(); i++)
        m_outputName = net->getOutput(i)->getName();

    // 设置输入维度
    net->getInput(0)->setDimensions(Dims4{1, 6, m_height, m_width});

    // 优化 profile (固定尺寸)
    IOptimizationProfile* profile = builder->createOptimizationProfile();
    profile->setDimensions(m_inputName.c_str(), OptProfileSelector::kMIN,
                           Dims4(1, 6, m_height, m_width));
    profile->setDimensions(m_inputName.c_str(), OptProfileSelector::kOPT,
                           Dims4(1, 6, m_height, m_width));
    profile->setDimensions(m_inputName.c_str(), OptProfileSelector::kMAX,
                           Dims4(1, 6, m_height, m_width));

    IBuilderConfig* cfg = builder->createBuilderConfig();
    cfg->addOptimizationProfile(profile);
    cfg->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, 1ULL << 30);

    if (builder->platformHasFastFp16()) {
        cfg->setFlag(BuilderFlag::kFP16);
        LOG("启用 FP16 模式");
    }

    IHostMemory* serialized = builder->buildSerializedNetwork(*net, *cfg);
    if (!serialized) {
        LOG("buildSerializedNetwork 失败");
        cfg->destroy(); parser->destroy(); net->destroy(); builder->destroy();
        return false;
    }

    m_trtRuntime = createInferRuntime(g_trtLogger);
    m_trtEngine  = ((IRuntime*)m_trtRuntime)->deserializeCudaEngine(
        serialized->data(), serialized->size());
    m_trtContext = ((ICudaEngine*)m_trtEngine)->createExecutionContext();

    // 缓存到磁盘
    std::ofstream outCache(cachePath, std::ios::binary);
    outCache.write((const char*)serialized->data(), serialized->size());
    outCache.close();

    serialized->destroy();
    cfg->destroy();
    parser->destroy();
    net->destroy();
    builder->destroy();

    m_engineReady = true;
    LOG("TensorRT engine 构建完成，已缓存");
    return true;
}

// ============================================================
// ProcessFrame
// ============================================================
bool FrameInterpolatorRIFE::ProcessFrame(
    uint64_t rgbaSrc, double timestamp,
    uint64_t& outRGBA, bool& outFrameRepeat)
{
    if (!m_initialized || !m_engineReady) return false;
    outFrameRepeat = false;
    cuCtxSetCurrent(m_cuContext);

    // 首帧: 仅缓存，无输出
    if (!m_hasPrev) {
        cuMemcpyDtoD((CUdeviceptr)m_prevRgba, (CUdeviceptr)rgbaSrc,
                     (size_t)m_width * m_height * 4);
        m_hasPrev = true;
        LOG("ProcessFrame: 首帧缓存");
        return false;
    }

    // Step 1: prev RGBA → RGB float (6ch 前 3ch)
    launch_rgba_to_rgb_float(
        (const uint8_t*)m_prevRgba, m_width * 4,
        (float*)m_rgbFloat6ch, m_width, m_height, 0, 0);

    // Step 2: curr RGBA → RGB float (6ch 后 3ch)
    launch_rgba_to_rgb_float(
        (const uint8_t*)rgbaSrc, m_width * 4,
        (float*)m_rgbFloat6ch, m_width, m_height, 3, 0);

    cudaStreamSynchronize(0);

    // Step 3: TensorRT 推理
    {
        using namespace nvinfer1;
        IExecutionContext* ctx = (IExecutionContext*)m_trtContext;
        ICudaEngine* eng = (ICudaEngine*)m_trtEngine;

        // TensorRT 8-10 兼容方式: enqueueV2
        int inIdx  = eng->getBindingIndex(m_inputName.c_str());
        int outIdx = eng->getBindingIndex(m_outputName.c_str());
        void* bindings[8] = {};
        bindings[inIdx]  = (void*)m_rgbFloat6ch;
        bindings[outIdx] = (void*)m_rgbFloatOut;

        if (!ctx->enqueueV2(bindings, 0, nullptr)) {
            LOG("TensorRT 推理失败");
            return false;
        }
    }
    cudaStreamSynchronize(0);

    // Step 4: RGB float → RGBA
    launch_rgb_float_to_rgba(
        (const float*)m_rgbFloatOut, m_width, m_height,
        (uint8_t*)m_rgbaOutput, m_width * 4, 0);
    cudaStreamSynchronize(0);

    // Step 5: 更新 prev 缓存
    cuMemcpyDtoD((CUdeviceptr)m_prevRgba, (CUdeviceptr)rgbaSrc,
                 (size_t)m_width * m_height * 4);

    outRGBA = m_rgbaOutput;
    return true;
}

// ============================================================
int FrameInterpolatorRIFE::GetExpectedOutputFrames(int inputFrames) const {
    if (inputFrames <= 0) return 0;
    if (inputFrames == 1) return 1;
    return inputFrames * 2 - 1;
}

// ============================================================
void FrameInterpolatorRIFE::Shutdown() {
    m_initialized = false;
    m_engineReady = false;
    m_hasPrev = false;
    DestroyEngine();

    auto sf = [](uint64_t& p) { if (p) { cuMemFree((CUdeviceptr)p); p = 0; } };
    sf(m_prevRgba); sf(m_rgbFloat6ch); sf(m_rgbFloatOut); sf(m_rgbaOutput);

    if (m_cuContext) {
        CUcontext prev = nullptr;
        cuCtxPushCurrent(m_cuContext); cuCtxPopCurrent(&prev);
        cuDevicePrimaryCtxRelease(m_cuDevice);
        m_cuContext = nullptr;
    }
    LOG("FrameInterpolatorRIFE 已关闭");
}

void FrameInterpolatorRIFE::DestroyEngine() {
    if (m_trtContext) { ((nvinfer1::IExecutionContext*)m_trtContext)->destroy(); m_trtContext = nullptr; }
    if (m_trtEngine)  { ((nvinfer1::ICudaEngine*)m_trtEngine)->destroy();  m_trtEngine  = nullptr; }
    if (m_trtRuntime) { ((nvinfer1::IRuntime*)m_trtRuntime)->destroy();    m_trtRuntime  = nullptr; }
}
