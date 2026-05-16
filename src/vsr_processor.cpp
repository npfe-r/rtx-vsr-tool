#include "vsr_processor.h"
#include "rtx_video_api.h"
#include "utils.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <windows.h>

#include "debug_util.h"

struct VSRProcessor::Impl {
    CUdevice   cuDevice  = 0;
    CUcontext  cuContext = nullptr;
};

// NGX is initialised at most once per process lifetime.
// NVSDK_NGX_CUDA_Init after NVSDK_NGX_CUDA_Shutdown in the same process is not
// guaranteed to produce a functional NGX state — the second init can succeed
// on the surface yet crash on the first ProcessFrame call (access violation
// inside the NGX driver component).  We therefore defer the real NGX shutdown
// to GlobalShutdown() (process exit) and keep the cuda_api_impl singleton
// alive across pipeline runs.
static bool s_ngxInitialized = false;
static int  s_ngxGpuIndex    = -1;

VSRProcessor::VSRProcessor() : m(new Impl) {}
VSRProcessor::~VSRProcessor() { Shutdown(); delete m; }

bool VSRProcessor::Initialize(int gpuIndex) {
    Shutdown();

    char buf[256];

    // Init CUDA driver API (idempotent after the first call)
    snprintf(buf, sizeof(buf), "VSR: cuInit(0)...");
    LogMsg("VSR: ",buf);
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) { LogMsg("VSR: ","VSR: cuInit failed"); return false; }
    LogMsg("VSR: ","VSR: cuInit OK");

    snprintf(buf, sizeof(buf), "VSR: cuDeviceGet(%d)...", gpuIndex);
    LogMsg("VSR: ",buf);
    res = cuDeviceGet(&m->cuDevice, gpuIndex);
    if (res != CUDA_SUCCESS) { LogMsg("VSR: ","VSR: cuDeviceGet failed"); return false; }
    LogMsg("VSR: ","VSR: cuDeviceGet OK");

    res = cuDevicePrimaryCtxRetain(&m->cuContext, m->cuDevice);
    if (res != CUDA_SUCCESS) { LogMsg("VSR: ","VSR: cuDevicePrimaryCtxRetain failed"); return false; }
    LogMsg("VSR: ","VSR: cuDevicePrimaryCtxRetain OK");

    if (!s_ngxInitialized) {
        // First and only NGX initialisation — once per process lifetime.
        CUcontext current;
        cuCtxPushCurrent(m->cuContext);

        LogMsg("VSR: ","VSR: calling rtx_video_api_cuda_create...");
        API_BOOL ok = rtx_video_api_cuda_create(
            m->cuContext,           // CUDA context
            nullptr,                // CUDA stream (null = default)
            gpuIndex,               // GPU index
            API_BOOL_FAIL,          // TrueHDR disabled
            API_BOOL_SUCCESS        // VSR enabled
        );

        cuCtxPopCurrent(&current);

        if (ok != API_BOOL_SUCCESS) {
            LogMsg("VSR: ","VSR: rtx_video_api_cuda_create failed");
            cuDevicePrimaryCtxRelease(m->cuDevice);
            m->cuContext = nullptr;
            return false;
        }

        s_ngxInitialized = true;
        s_ngxGpuIndex    = gpuIndex;
        LogMsg("VSR: ","VSR: NGX initialised (first time)");
    } else if (s_ngxGpuIndex != gpuIndex) {
        // GPU selection changed — the existing NGX/VSR state is tied to the
        // previous GPU.  Do a full re-init (same risk as above, but GPU
        // switching between runs is rare and the user explicitly chose it).
        LogMsg("VSR: ","VSR: GPU changed, performing full re-init");
        GlobalShutdown();

        CUcontext current;
        cuCtxPushCurrent(m->cuContext);
        API_BOOL ok = rtx_video_api_cuda_create(
            m->cuContext, nullptr, gpuIndex,
            API_BOOL_FAIL, API_BOOL_SUCCESS);
        cuCtxPopCurrent(&current);

        if (ok != API_BOOL_SUCCESS) {
            LogMsg("VSR: ","VSR: rtx_video_api_cuda_create (re-init) failed");
            cuDevicePrimaryCtxRelease(m->cuDevice);
            m->cuContext = nullptr;
            return false;
        }

        s_ngxInitialized = true;
        s_ngxGpuIndex    = gpuIndex;
        LogMsg("VSR: ","VSR: NGX re-initialised on new GPU");
    }
    // Same GPU, NGX already initialised: reuse the existing cuda_api_impl
    // singleton and VSR feature handle.  The evaluate function recreates
    // internal CUDA arrays lazily if frame dimensions changed, so no
    // re-creation of the VSR feature is needed here.

    m_initialized = true;
    LogMsg("VSR: ","VSR: initialized successfully");
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
        // Release the CUDA primary context reference.  We do NOT call
        // rtx_video_api_cuda_shutdown() here — NGX must stay alive for the
        // entire process lifetime (see the comment at the top of this file).
        if (m->cuContext) {
            cuDevicePrimaryCtxRelease(m->cuDevice);
            m->cuContext = nullptr;
        }
        m_initialized = false;
    }
}

void VSRProcessor::GlobalShutdown() {
    if (s_ngxInitialized) {
        // NGX shutdown requires a current CUDA context on the calling thread.
        // Retain the primary context temporarily, push it, then clean up.
        CUdevice dev;
        CUcontext ctx = nullptr;
        if (cuDeviceGet(&dev, s_ngxGpuIndex) == CUDA_SUCCESS &&
            cuDevicePrimaryCtxRetain(&ctx, dev) == CUDA_SUCCESS)
        {
            CUcontext prev;
            cuCtxPushCurrent(ctx);
            rtx_video_api_cuda_shutdown();
            cuCtxPopCurrent(&prev);
            cuDevicePrimaryCtxRelease(dev);
        }
        s_ngxInitialized = false;
    }
}
