#pragma once
#include <cstdint>
#include <functional>

class FrameInterpolator {
public:
    FrameInterpolator();
    ~FrameInterpolator();

    // Check if NvOFFRUC.dll exists alongside the executable.
    static bool IsDllPresent();

    // Initialize FRUC for the given output (VSR-upscaled) resolution.
    // width, height: VSR output dimensions.
    // gpuIndex: CUDA device ordinal.
    // colorMatrix: ColorMatrix enum for YUV↔RGB conversion (COLOR_MATRIX_BT709 etc.).
    // Returns false if NvOFFRUC is unavailable, init fails, or GPU unsupported.
    bool Initialize(int width, int height, int gpuIndex, int colorMatrix = 1);

    // Process one VSR output frame. Must be called for every VSR-processed frame.
    // vsrRgba: CUdeviceptr to the VSR d_rgba_dst RGBA output (8-bit, byte 0=R).
    // timestamp: frame timestamp in seconds (used by FRUC for temporal interpolation).
    //            Must be monotonically increasing.  Typically frame_seq / src_fps.
    // outInterpolatedPtr: [out] CUdeviceptr to the interpolated RGBA frame
    //                     (byte 0=R, 1=G, 2=B, 3=A, matching the pipeline).
    // outFrameRepeat: [out] true if FRUC detected a scene cut and repeated a frame.
    // Returns true if an interpolated frame is available (should be encoded).
    // Returns false for the first frame (FRUC needs two frames to interpolate).
    bool ProcessFrame(uint64_t vsrRgba, double timestamp,
                      uint64_t& outInterpolatedPtr, bool& outFrameRepeat);

    // Same as ProcessFrame but returns FRUC NV12 output directly (skips NV12→RGBA).
    // For use with the encoder's GPU zero-copy path — D2D copy to encoder NV12 buffers.
    bool ProcessFrameNV12(uint64_t vsrRgba, double timestamp,
                          uint64_t& outNV12Ptr, int& outNV12Pitch,
                          bool& outFrameRepeat);

    // Get the total number of output frames for the pipeline given input frame count.
    // For 2x: inputFrames * 2 - 1 (first frame has no interpolated predecessor).
    int GetExpectedOutputFrames(int inputFrames) const;

    // Shutdown FRUC, unregister resources, free GPU memory, unload DLL.
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

    // Error/status callback
    std::function<void(const char*)> onLog;

private:
    bool LoadDLL();
    void FreeDLL();

    // DLL handle and function pointers
    void* m_hDLL = nullptr;

    // NvOFFRUC function pointers (__stdcall matching DLL exports)
    typedef int (__stdcall* PtrNvCreate)(void*, void*);
    typedef int (__stdcall* PtrNvReg)(void*, const void*);
    typedef int (__stdcall* PtrNvUnreg)(void*, const void*);
    typedef int (__stdcall* PtrNvProcess)(void*, const void*, const void*);
    typedef int (__stdcall* PtrNvDestroy)(void*);

    PtrNvCreate  m_NvOFFRUCCreate  = nullptr;
    PtrNvReg     m_NvOFFRUCRegisterResource = nullptr;
    PtrNvUnreg   m_NvOFFRUCUnregisterResource = nullptr;
    PtrNvProcess m_NvOFFRUCProcess = nullptr;
    PtrNvDestroy m_NvOFFRUCDestroy = nullptr;

    void* m_hFRUC = nullptr;

    // FRUC NV12 buffers (2 render input + 1 interpolate output)
    uint64_t m_renderResources[2]  = {0, 0};
    uint64_t m_interpolateResource = 0;

    // RGBA output buffer — FRUC outputs NV12, we convert to RGBA for the pipeline
    uint64_t m_rgbaOutput = 0;

    int m_width  = 0;
    int m_height = 0;
    int m_colorMatrix = 1;         // ColorMatrix enum (BT.709 default)
    int m_currentRenderIndex = 0;
    bool m_firstFrame = true;
    bool m_initialized = false;
    int m_gpuIndex = 0;
};
