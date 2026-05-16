#pragma once
#include <cstdint>

enum class VSRQuality : int {
    Bicubic = 0,
    Low     = 1,
    Medium  = 2,
    High    = 3,
    Ultra   = 4
};

class VSRProcessor {
public:
    VSRProcessor();
    ~VSRProcessor();

    bool Initialize(int gpuIndex, bool enableTrueHdr = false);
    bool ProcessFrame(const void* srcDevicePtr, void* dstDevicePtr,
                      int srcW, int srcH, int dstW, int dstH,
                      VSRQuality quality);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // Process-level NGX shutdown — called once at app exit.
    // NGX does not support clean re-initialisation in the same process,
    // so rtx_video_api_cuda_shutdown() must be deferred to process exit
    // rather than called between pipeline runs.
    static void GlobalShutdown();

private:
    struct Impl;
    Impl* m;
    bool m_initialized = false;
};
