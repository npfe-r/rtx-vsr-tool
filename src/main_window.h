#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "pipeline_ctrl.h"
#include "config.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    void Render();
    HWND Handle() const { return m_hWnd; }

    // Start pipeline from current config (used with -autostart command-line arg)
    bool AutoStart();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool    InitD3D11();
    void    CleanupD3D11();
    void    CreateRenderTarget();
    void    CleanupRenderTarget();
    void    RenderUI();

    void    OnSelectInput();
    void    OnSelectOutput();
    void    OnStartStop();
    void    OnPauseResume();
    void    OnPipelineProgress(const PipelineProgress& p);
    void    OnPipelineError(const wchar_t* msg);
    void    OnPipelineCompleted();
    void    OnPipelineStatus(const char* msg);
    void    LoadConfigToUI();
    void    SaveUIToConfig();
    void    EnumGpus();
    void    UpdateOutputExtension();

    HWND      m_hWnd   = nullptr;
    HINSTANCE m_hInst  = nullptr;

    ID3D11Device*           m_d3dDevice  = nullptr;
    ID3D11DeviceContext*    m_d3dContext = nullptr;
    IDXGISwapChain*         m_swapChain  = nullptr;
    ID3D11RenderTargetView* m_rtv        = nullptr;

    char    m_inputPath[512]   = {};
    char    m_outputPath[512]  = {};
    char    m_inputInfo[256]   = {};
    int     m_qualityLevel     = 3;
    int     m_outputMode       = 0;
    int     m_outputWidth      = 3840;
    int     m_outputHeight     = 2160;
    int     m_encoderIndex     = 0;
    int     m_crf              = 18;
    int     m_encoderSpeed     = 3;
    int     m_gpuIndex         = 0;
    int     m_containerFormat  = 0;
    int     m_audioMode        = 1;
    int     m_audioBitrate     = 128;
    int     m_trueHdrEnabled   = 0;
    int     m_thdrContrast    = 100;
    int     m_thdrSaturation  = 100;
    int     m_thdrMiddleGray  = 50;
    int     m_thdrMaxLuminance = 1000;
    int     m_frameInterpolation = 0;
    float   m_progressPct      = 0.0f;
    char    m_statusText[256]    = {};
    char    m_encoderWarning[256] = {};
    char    m_decodeMode[16]     = {};

    PipelineController m_pipeline;
    Config             m_config;
    VideoInfo          m_videoInfo;
    bool               m_isRunning         = false;
    bool               m_isPaused          = false;
    bool               m_showCompletePopup = false;
    bool               m_hasError          = false;

    std::vector<std::string> m_gpuNames;
    bool m_autoSized = false;
    bool m_gpuDecodeAvailable = true;     // set by OnSelectInput probe
    bool m_nvdecProbed = false;           // true after NVDEC availability check
    char m_fallbackMsg[256] = {};         // non-empty = fallback to warn about

    std::mutex m_progressMutex;
    std::chrono::steady_clock::time_point m_startTime;
    char m_completeStats[256] = {};
    float      m_displayFps      = 0.0f;
    float      m_displayEta      = 0.0f;
    int        m_currentFrame    = 0;
    int        m_totalFrames     = 0;
    float      m_smoothedMs      = 0.0f; // EMA-smoothed ms-per-frame for stable FPS/ETA
};