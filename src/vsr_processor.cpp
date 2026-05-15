#include "vsr_processor.h"
#include "rtx_video_api.h"
#include "utils.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <windows.h>

static void DbgMsg(const char* msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "VSR: %s\n", msg); fflush(f); fclose(f); }

    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}

struct VSRProcessor::Impl {
    CUdevice   cuDevice  = 0;
    CUcontext  cuContext = nullptr;
};

VSRProcessor::VSRProcessor() : m(new Impl) {}
VSRProcessor::~VSRProcessor() { Shutdown(); delete m; }

bool VSRProcessor::Initialize(int gpuIndex) {
    Shutdown();

    char buf[256];

    // Init CUDA driver API
    snprintf(buf, sizeof(buf), "VSR: cuInit(0)...");
    DbgMsg(buf);
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) { DbgMsg("VSR: cuInit failed"); return false; }
    DbgMsg("VSR: cuInit OK");

    snprintf(buf, sizeof(buf), "VSR: cuDeviceGet(%d)...", gpuIndex);
    DbgMsg(buf);
    res = cuDeviceGet(&m->cuDevice, gpuIndex);
    if (res != CUDA_SUCCESS) { DbgMsg("VSR: cuDeviceGet failed"); return false; }
    DbgMsg("VSR: cuDeviceGet OK");

    res = cuDevicePrimaryCtxRetain(&m->cuContext, m->cuDevice);
    if (res != CUDA_SUCCESS) { DbgMsg("VSR: cuDevicePrimaryCtxRetain failed"); return false; }
    DbgMsg("VSR: cuDevicePrimaryCtxRetain OK");

    // Push CUDA context for this thread
    CUcontext current;
    cuCtxPushCurrent(m->cuContext);

    DbgMsg("VSR: calling rtx_video_api_cuda_create...");
    // rtx_video_api_cuda_create handles: NGX init, capability check, VSR feature creation
    API_BOOL ok = rtx_video_api_cuda_create(
        m->cuContext,           // CUDA context
        nullptr,                // CUDA stream (null = default)
        gpuIndex,               // GPU index
        API_BOOL_FAIL,          // TrueHDR disabled
        API_BOOL_SUCCESS        // VSR enabled
    );

    cuCtxPopCurrent(&current);

    if (ok != API_BOOL_SUCCESS) {
        DbgMsg("VSR: rtx_video_api_cuda_create failed");
        cuDevicePrimaryCtxRelease(m->cuDevice);
        m->cuContext = nullptr;
        return false;
    }

    m_initialized = true;
    DbgMsg("VSR: initialized successfully");
    return true;
}

bool VSRProcessor::ProcessFrame(const void* srcDevicePtr, void* dstDevicePtr,
                                 int srcW, int srcH, int dstW, int dstH,
                                 VSRQuality quality) {
    if (!m_initialized) return false;

    CUcontext current;
    cuCtxPushCurrent(m->cuContext);

    API_RECT inputRect  = { 0, 0, (uint32_t)srcW, (uint32_t)srcH };
    API_RECT outputRect = { 0, 0, (uint32_t)dstW, (uint32_t)dstH };

    API_VSR_Setting vsrSetting;
    vsrSetting.QualityLevel = (int)quality;

    bool ok = rtx_video_api_cuda_evaluate_deviceptr(
        const_cast<void*>(srcDevicePtr),
        dstDevicePtr,
        inputRect,
        outputRect,
        &vsrSetting,
        nullptr  // no TrueHDR settings
    );

    cuCtxPopCurrent(&current);
    return ok == API_BOOL_SUCCESS;
}

void VSRProcessor::Shutdown() {
    if (m_initialized) {
        CUcontext current;
        cuCtxPushCurrent(m->cuContext);
        rtx_video_api_cuda_shutdown();
        cuCtxPopCurrent(&current);

        if (m->cuContext) {
            cuDevicePrimaryCtxRelease(m->cuDevice);
            m->cuContext = nullptr;
        }
        m_initialized = false;
    }
}
