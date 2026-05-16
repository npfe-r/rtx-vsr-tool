#pragma once
#include <windows.h>
#include <string>

struct VSRConfig {
    // Window state
    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;
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
                                    // 3=libx264, 4=libx265, 5=libaom-av1
    int crf = 18;
    int encoderSpeed = 2;           // 0=fast ... 4=slow
    int gpuIndex = 0;
    int containerFormat = 0;        // 0=mp4, 1=mov
    int outputFps = 0;              // 0=source fps
    int audioMode = 1;              // 0=no audio, 1=copy source, 2=AAC encode
    int audioBitrate = 128;         // kbps: 64, 96, 128, 192, 256, 320
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
