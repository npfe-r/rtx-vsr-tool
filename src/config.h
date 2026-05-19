#pragma once
#include <windows.h>
#include <string>

struct VSRConfig {
    // Window state
    int windowW = 540;
    int windowH = 506;

    // Last used paths
    wchar_t lastInputPath[MAX_PATH] = L"";
    wchar_t lastOutputPath[MAX_PATH] = L"";

    // Processing params
    int qualityLevel = 3;           // 0=Bicubic ... 4=Ultra
    int outputMode = 0;             // 0=2x, 1=4x, 2=fixed
    int fixedWidth = 3840;
    int fixedHeight = 2160;
    int encoderIndex = 0;           // 0=H.264 NVENC, 1=H.265 NVENC, 2=AV1 NVENC,
                                    // 3=libx264, 4=libx265, 5=libaom-av1, 6=SVT-AV1
    int crf = 18;
    int encoderSpeed = 3;           // 0=fast ... 4=slow
    int gpuIndex = 0;
    int containerFormat = 0;        // 0=mp4, 1=mov
    int audioMode = 1;              // 0=no audio, 1=copy source, 2=AAC encode
    int audioBitrate = 128;         // kbps: 64, 96, 128, 192, 256, 320
    int trueHdrEnabled = 0;         // 0=disabled, 1=enabled (TrueHDR / HDR tone mapping)
    int frameInterpolation = 0;     // 0=off, 1=2x (NvOFFRUC frame interpolation)
    int frucPosition = 0;           // 0=后插帧(After VSR), 1=前插帧(Before VSR)
    int thdrContrast = 100;         // 0–200
    int thdrSaturation = 100;       // 0–200
    int thdrMiddleGray = 50;        // 10–100
    int thdrMaxLuminance = 1000;    // 400–2000
};

class Config {
public:
    Config();
    ~Config();

    void Load();
    void Save();

    VSRConfig& Get() { return m_config; }

private:
    std::wstring GetIniPath() const;
    VSRConfig m_config;
};
