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

    bool Initialize(int gpuIndex);
    bool ProcessFrame(const void* srcDevicePtr, void* dstDevicePtr,
                      int srcW, int srcH, int dstW, int dstH,
                      VSRQuality quality);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

private:
    struct Impl;
    Impl* m;
    bool m_initialized = false;
};
