#include "frame_interpolator.h"

#include <windows.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdio>

#include "debug_util.h"

// CUDA kernel declarations (from cuda_yuv.cu, declared in pipeline_ctrl.h)
extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix);

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix);

// ============================================================================
// NvOFFRUC API — loaded dynamically from NvOFFRUC.dll
// ============================================================================

#define NvOFFRUC_MAX_RESOURCE 10
#define NvOFFRUC_MIN_RESOURCE 3

enum NvOFFRUCCUDAResourceType {
    CudaResourceTypeUndefined = -1,
    CudaResourceCuDevicePtr,
    CudaResourceCuArray
};

enum NvOFFRUCResourceType {
    UndefinedResourceType = -1,
    CudaResource = 0,
    DirectX11Resource = 1,
};

enum NvOFFRUCSurfaceFormat {
    UndefinedSurfaceType = -1,
    NV12Surface = 0,         // <-- changed: was ARGBSurface=1
    ARGBSurface = 1,
};

typedef struct _NvOFFRUCHandle_st { int dummy; }* NvOFFRUCHandle;

typedef int (__stdcall* PFN_NvOFFRUCCreate)(const void*, NvOFFRUCHandle*);
typedef int (__stdcall* PFN_NvOFFRUCRegisterResource)(NvOFFRUCHandle, const void*);
typedef int (__stdcall* PFN_NvOFFRUCUnregisterResource)(NvOFFRUCHandle, const void*);
typedef int (__stdcall* PFN_NvOFFRUCProcess)(NvOFFRUCHandle, const void*, const void*);
typedef int (__stdcall* PFN_NvOFFRUCDestroy)(NvOFFRUCHandle);

#define NvOFFRUC_SUCCESS 0

// NvOFFRUC structs — same layout, no changes needed
struct NvOFFRUC_CreateParams {
    uint32_t    uiWidth;
    uint32_t    uiHeight;
    void*       pDevice;
    int         eResourceType;       // NvOFFRUCResourceType
    int         eSurfaceFormat;      // NvOFFRUCSurfaceFormat
    int         eCUDAResourceType;   // NvOFFRUCCUDAResourceType
    uint32_t    uiReserved[32];
};

struct NvOFFRUC_FrameData {
    void*       pFrame;
    double      nTimeStamp;
    size_t      nCuSurfacePitch;
    bool*       bHasFrameRepetitionOccurred;
    uint32_t    uiReserved[32];
};

struct NvOFFRUC_ProcessInParams {
    NvOFFRUC_FrameData stFrameDataInput;
    uint32_t    bSkipWarp : 1;
    union {
        struct { uint64_t uiFenceValueToWaitOn; } FenceWaitValue;
        struct {
            uint64_t uiKeyForRenderTextureAcquire;
            uint64_t uiKeyForInterpTextureAcquire;
        } MutexAcquireKey;
    } uSyncWait;
    uint32_t    uiReserved[32];
};

struct NvOFFRUC_ProcessOutParams {
    NvOFFRUC_FrameData stFrameDataOutput;
    union {
        struct { uint64_t uiFenceValueToSignalOn; } FenceSignalValue;
        struct {
            uint64_t uiKeyForRenderTextureRelease;
            uint64_t uiKeyForInterpolateRelease;
        } MutexReleaseKey;
    } uSyncSignal;
    uint32_t    uiReserved[32];
};

struct NvOFFRUC_RegisterResourceParams {
    void* pArrResource[NvOFFRUC_MAX_RESOURCE];
    void* pD3D11FenceObj;
    uint32_t uiCount;
};

struct NvOFFRUC_UnregisterResourceParams {
    void* pArrResource[NvOFFRUC_MAX_RESOURCE];
    uint32_t uiCount;
};

// ============================================================================
// Helpers
// ============================================================================

static void Log(const char* msg) {
    LogMsg("FRUC: ", msg);
}

// ============================================================================
// FrameInterpolator implementation
// ============================================================================

FrameInterpolator::FrameInterpolator() {}
FrameInterpolator::~FrameInterpolator() { Shutdown(); }

bool FrameInterpolator::IsDllPresent() {
    HMODULE h = LoadLibraryW(L"NvOFFRUC.dll");
    if (h) {
        FreeLibrary(h);
        return true;
    }
    return false;
}

bool FrameInterpolator::LoadDLL() {
    if (m_hDLL) return true;

    m_hDLL = (void*)LoadLibraryW(L"NvOFFRUC.dll");
    if (!m_hDLL) {
        Log("NvOFFRUC.dll not found — frame interpolation unavailable");
        return false;
    }

    auto getProc = [&](const char* name) -> void* {
        return (void*)GetProcAddress((HMODULE)m_hDLL, name);
    };

    m_NvOFFRUCCreate  = reinterpret_cast<PtrNvCreate>(getProc("NvOFFRUCCreate"));
    m_NvOFFRUCRegisterResource = reinterpret_cast<PtrNvReg>(getProc("NvOFFRUCRegisterResource"));
    m_NvOFFRUCUnregisterResource = reinterpret_cast<PtrNvUnreg>(getProc("NvOFFRUCUnregisterResource"));
    m_NvOFFRUCProcess = reinterpret_cast<PtrNvProcess>(getProc("NvOFFRUCProcess"));
    m_NvOFFRUCDestroy = reinterpret_cast<PtrNvDestroy>(getProc("NvOFFRUCDestroy"));

    if (!m_NvOFFRUCCreate || !m_NvOFFRUCRegisterResource ||
        !m_NvOFFRUCUnregisterResource || !m_NvOFFRUCProcess || !m_NvOFFRUCDestroy) {
        Log("NvOFFRUC.dll missing required exports");
        FreeDLL();
        return false;
    }

    Log("NvOFFRUC.dll loaded successfully");
    return true;
}

void FrameInterpolator::FreeDLL() {
    if (m_hDLL) {
        FreeLibrary((HMODULE)m_hDLL);
        m_hDLL = nullptr;
    }
    m_NvOFFRUCCreate = nullptr;
    m_NvOFFRUCRegisterResource = nullptr;
    m_NvOFFRUCUnregisterResource = nullptr;
    m_NvOFFRUCProcess = nullptr;
    m_NvOFFRUCDestroy = nullptr;
}

bool FrameInterpolator::Initialize(int width, int height, int gpuIndex, int colorMatrix) {
    Shutdown();

    m_width  = width;
    m_height = height;
    m_gpuIndex = gpuIndex;
    m_colorMatrix = colorMatrix;

    // 1. Load DLL
    if (!LoadDLL()) return false;

    // 2. Ensure CUDA context is set for this device
    cudaError_t ce = cudaSetDevice(gpuIndex);
    if (ce != cudaSuccess) {
        Log("Initialize: cudaSetDevice failed");
        FreeDLL();
        return false;
    }

    CUdevice cuDevice;
    if (cuDeviceGet(&cuDevice, gpuIndex) != CUDA_SUCCESS) {
        Log("Initialize: cuDeviceGet failed");
        FreeDLL();
        return false;
    }
    CUcontext cuCtx;
    if (cuDevicePrimaryCtxRetain(&cuCtx, cuDevice) != CUDA_SUCCESS) {
        Log("Initialize: cuDevicePrimaryCtxRetain failed");
        FreeDLL();
        return false;
    }
    cuCtxSetCurrent(cuCtx);

    // 3. Allocate CUDA device memory
    //    FRUC uses NV12 format internally (matching ffmpeg-nvinterpolate).
    //    NV12 buffer layout: Y plane (w*h bytes) + UV interleaved (w*h/2 bytes)
    //    We also allocate an RGBA output buffer for the pipeline encoding path.
    size_t nv12Size   = (size_t)m_width * m_height * 3 / 2;
    size_t rgbaSize   = (size_t)m_width * m_height * 4;

    for (int i = 0; i < 2; i++) {
        CUresult r = cuMemAlloc((CUdeviceptr*)&m_renderResources[i], nv12Size);
        if (r != CUDA_SUCCESS) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Initialize: cuMemAlloc render[%d] NV12 failed: %d", i, (int)r);
            Log(buf);
            Shutdown();
            return false;
        }
    }
    {
        CUresult r = cuMemAlloc((CUdeviceptr*)&m_interpolateResource, nv12Size);
        if (r != CUDA_SUCCESS) {
            Log("Initialize: cuMemAlloc interpolate NV12 failed");
            Shutdown();
            return false;
        }
    }
    {
        CUresult r = cuMemAlloc((CUdeviceptr*)&m_rgbaOutput, rgbaSize);
        if (r != CUDA_SUCCESS) {
            Log("Initialize: cuMemAlloc rgbaOutput failed");
            Shutdown();
            return false;
        }
    }

    // 4. Create FRUC instance with NV12 surface format
    NvOFFRUC_CreateParams createParams = {};
    createParams.uiWidth           = (uint32_t)m_width;
    createParams.uiHeight          = (uint32_t)m_height;
    createParams.pDevice           = nullptr;          // CUDA path: no D3D device
    createParams.eResourceType     = CudaResource;
    createParams.eSurfaceFormat    = NV12Surface;      // NV12, matching ffmpeg-nvinterpolate
    createParams.eCUDAResourceType = CudaResourceCuDevicePtr;

    NvOFFRUCHandle hFRUC = nullptr;
    int status = m_NvOFFRUCCreate(&createParams, &hFRUC);
    if (status != NvOFFRUC_SUCCESS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "NvOFFRUCCreate failed, status=%d", status);
        Log(buf);
        Shutdown();
        return false;
    }
    m_hFRUC = (void*)hFRUC;

    // 5. Register resources with FRUC
    //    Layout: [interp, render0, render1] — interp first, then render
    void* resources[3];
    resources[0] = (void*)&m_interpolateResource;
    resources[1] = (void*)&m_renderResources[0];
    resources[2] = (void*)&m_renderResources[1];

    NvOFFRUC_RegisterResourceParams regParams = {};
    regParams.pArrResource[0] = resources[0];
    regParams.pArrResource[1] = resources[1];
    regParams.pArrResource[2] = resources[2];
    regParams.uiCount = 3;
    regParams.pD3D11FenceObj = nullptr;

    status = m_NvOFFRUCRegisterResource(hFRUC, &regParams);
    if (status != NvOFFRUC_SUCCESS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "NvOFFRUCRegisterResource failed, status=%d", status);
        Log(buf);
        Shutdown();
        return false;
    }

    m_currentRenderIndex = 0;
    m_firstFrame = true;
    m_initialized = true;

    Log("FrameInterpolator initialized (2x FRUC, NV12 surface)");
    return true;
}

bool FrameInterpolator::ProcessFrame(uint64_t vsrRgba, double timestamp,
                                     uint64_t& outInterpolatedPtr,
                                     bool& outFrameRepeat) {
    if (!m_initialized || !m_hFRUC) return false;

    outFrameRepeat = false;

    // --- Step 1: Convert VSR RGBA output → NV12 in the current render resource ---
    // FRUC operates on NV12 data.  We convert the VSR RGBA output on the default
    // stream, then synchronise before calling NvOFFRUCProcess (FRUC uses its own
    // internal CUDA streams that don't synchronise with the default stream).
    //
    // NV12 buffer layout (linear CUDA memory):
    //   bytes [0            .. w*h)       : Y plane (luma)
    //   bytes [w*h          .. w*h*3/2)   : UV interleaved (chroma)
    size_t yPlaneBytes = (size_t)m_width * m_height;
    uint64_t renderBuf = m_renderResources[m_currentRenderIndex];

    launch_rgba_to_nv12(
        (const uint8_t*)vsrRgba, m_width * 4,                 // src RGBA
        (uint8_t*)renderBuf, m_width,                          // dst Y plane
        (uint8_t*)(renderBuf + yPlaneBytes), m_width,          // dst UV plane
        m_width, m_height, 0, m_colorMatrix);                  // default stream

    // Synchronise default stream so the NV12 conversion completes before FRUC reads it
    cudaStreamSynchronize(0);

    // --- Step 2: Call NvOFFRUCProcess ---
    // FRUC compares the current input (just converted from RGBA) against the
    // internally-captured previous frame and outputs an interpolated NV12 frame.
    bool hasRepeat = false;

    NvOFFRUC_ProcessInParams inParams = {};
    inParams.stFrameDataInput.pFrame          = (void*)&m_renderResources[m_currentRenderIndex];
    inParams.stFrameDataInput.nTimeStamp      = timestamp;
    inParams.stFrameDataInput.nCuSurfacePitch = (size_t)m_width;
    inParams.bSkipWarp = m_firstFrame ? 1 : 0;

    NvOFFRUC_ProcessOutParams outParams = {};
    outParams.stFrameDataOutput.pFrame          = (void*)&m_interpolateResource;
    outParams.stFrameDataOutput.nTimeStamp      = timestamp;
    outParams.stFrameDataOutput.nCuSurfacePitch = (size_t)m_width;
    outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &hasRepeat;

    int status = m_NvOFFRUCProcess((NvOFFRUCHandle)m_hFRUC, &inParams, &outParams);
    if (status != NvOFFRUC_SUCCESS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "NvOFFRUCProcess failed, status=%d", status);
        Log(buf);
        m_currentRenderIndex = 1 - m_currentRenderIndex;
        return false;
    }

    // First frame: bSkipWarp=1 initialises FRUC's internal optical flow state
    // but produces no interpolated output.
    if (m_firstFrame) {
        m_firstFrame = false;
        m_currentRenderIndex = 1 - m_currentRenderIndex;
        Log("ProcessFrame: first frame (bSkipWarp=1), FRUC state initialised");
        return false;
    }

    // --- Step 3: Wait for FRUC internal GPU work to complete ---
    // NvOFFRUCProcess submits work to its own internal CUDA streams which do
    // NOT synchronise with the default stream.  Without a full device sync,
    // the nv12_to_rgba conversion below may read stale / partially-written
    // interpolated frame data.
    cudaDeviceSynchronize();

    // --- Step 4: Convert FRUC NV12 output → RGBA for the encoding pipeline ---
    // The pipeline's encodeFrame() expects RGBA (byte 0=R, 1=G, 2=B, 3=A).
    launch_nv12_to_rgba(
        (const uint8_t*)m_interpolateResource, m_width,         // src Y plane
        (const uint8_t*)(m_interpolateResource + yPlaneBytes), m_width,  // src UV plane
        (uint8_t*)m_rgbaOutput, m_width * 4,                    // dst RGBA
        m_width, m_height, 0, m_colorMatrix);                   // default stream

    // Wait for the conversion kernel to complete before returning the pointer.
    cudaStreamSynchronize(0);

    outInterpolatedPtr = m_rgbaOutput;
    outFrameRepeat = hasRepeat;

    // Toggle render index for next frame
    m_currentRenderIndex = 1 - m_currentRenderIndex;

    Log("ProcessFrame: interpolated frame produced (NV12 conv)");
    return true;
}

bool FrameInterpolator::ProcessFrameNV12(uint64_t vsrRgba, double timestamp,
                                         uint64_t& outNV12Ptr, int& outNV12Pitch,
                                         bool& outFrameRepeat) {
    if (!m_initialized || !m_hFRUC) return false;

    outFrameRepeat = false;

    // Step 1: Convert VSR RGBA output → NV12 in the current render resource
    size_t yPlaneBytes = (size_t)m_width * m_height;
    uint64_t renderBuf = m_renderResources[m_currentRenderIndex];

    launch_rgba_to_nv12(
        (const uint8_t*)vsrRgba, m_width * 4,
        (uint8_t*)renderBuf, m_width,
        (uint8_t*)(renderBuf + yPlaneBytes), m_width,
        m_width, m_height, 0, m_colorMatrix);

    cudaStreamSynchronize(0);

    // Step 2: Call NvOFFRUCProcess
    bool hasRepeat = false;

    NvOFFRUC_ProcessInParams inParams = {};
    inParams.stFrameDataInput.pFrame          = (void*)&m_renderResources[m_currentRenderIndex];
    inParams.stFrameDataInput.nTimeStamp      = timestamp;
    inParams.stFrameDataInput.nCuSurfacePitch = (size_t)m_width;
    inParams.bSkipWarp = m_firstFrame ? 1 : 0;

    NvOFFRUC_ProcessOutParams outParams = {};
    outParams.stFrameDataOutput.pFrame          = (void*)&m_interpolateResource;
    outParams.stFrameDataOutput.nTimeStamp      = timestamp;
    outParams.stFrameDataOutput.nCuSurfacePitch = (size_t)m_width;
    outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &hasRepeat;

    int status = m_NvOFFRUCProcess((NvOFFRUCHandle)m_hFRUC, &inParams, &outParams);
    if (status != NvOFFRUC_SUCCESS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "NvOFFRUCProcess failed, status=%d", status);
        Log(buf);
        m_currentRenderIndex = 1 - m_currentRenderIndex;
        return false;
    }

    if (m_firstFrame) {
        m_firstFrame = false;
        m_currentRenderIndex = 1 - m_currentRenderIndex;
        Log("ProcessFrameNV12: first frame (bSkipWarp=1), FRUC state initialised");
        return false;
    }

    // Step 3: Wait for FRUC internal GPU work to complete
    cudaDeviceSynchronize();

    // Step 4: Return NV12 pointer directly (skip NV12→RGBA conversion)
    outNV12Ptr   = m_interpolateResource;
    outNV12Pitch = m_width;
    outFrameRepeat = hasRepeat;

    m_currentRenderIndex = 1 - m_currentRenderIndex;

    Log("ProcessFrameNV12: interpolated frame produced (NV12 direct)");
    return true;
}

int FrameInterpolator::GetExpectedOutputFrames(int inputFrames) const {
    if (inputFrames <= 0) return 0;
    if (inputFrames == 1) return 1;
    return inputFrames * 2 - 1;
}

void FrameInterpolator::Shutdown() {
    m_initialized = false;

    // Unregister and destroy FRUC
    if (m_hFRUC && m_NvOFFRUCDestroy && m_NvOFFRUCUnregisterResource) {
        void* resources[3];
        resources[0] = (void*)&m_interpolateResource;
        resources[1] = (void*)&m_renderResources[0];
        resources[2] = (void*)&m_renderResources[1];

        NvOFFRUC_UnregisterResourceParams unregParams = {};
        unregParams.pArrResource[0] = resources[0];
        unregParams.pArrResource[1] = resources[1];
        unregParams.pArrResource[2] = resources[2];
        unregParams.uiCount = 3;

        m_NvOFFRUCUnregisterResource((NvOFFRUCHandle)m_hFRUC, &unregParams);
        m_NvOFFRUCDestroy((NvOFFRUCHandle)m_hFRUC);
        m_hFRUC = nullptr;
    }

    // Free CUDA memory
    auto safeFree = [](uint64_t& ptr) {
        if (ptr) { cuMemFree((CUdeviceptr)ptr); ptr = 0; }
    };
    safeFree(m_renderResources[0]);
    safeFree(m_renderResources[1]);
    safeFree(m_interpolateResource);
    safeFree(m_rgbaOutput);

    m_firstFrame = true;
    m_currentRenderIndex = 0;

    FreeDLL();
    Log("FrameInterpolator shut down");
}
