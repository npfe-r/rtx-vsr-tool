#include "main_window.h"
#include <commdlg.h>
#include <shellapi.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cuda_runtime.h>

static void widen(const char* utf8, wchar_t* wbuf, int wbufSize)
{
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wbufSize);
}

static void narrow(const wchar_t* wstr, char* utf8buf, int bufSize)
{
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8buf, bufSize, nullptr, nullptr);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

MainWindow::MainWindow()
{
    m_pipeline.onProgress  = [this](const PipelineProgress& p) { OnPipelineProgress(p); };
    m_pipeline.onError     = [this](const wchar_t* msg)        { OnPipelineError(msg); };
    m_pipeline.onCompleted = [this]()                          { OnPipelineCompleted(); };
}

MainWindow::~MainWindow()
{
    m_pipeline.Stop();
    SaveUIToConfig();
    m_config.Save();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D11();
}

// ============================================================================
// D3D11 initialisation
// ============================================================================

bool MainWindow::InitD3D11()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags      = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hWnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed  = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &sd, &m_swapChain, &m_d3dDevice, &featureLevel, &m_d3dContext);

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "D3D11CreateDeviceAndSwapChain failed: HRESULT=0x%08lX", hr);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hCon && hCon != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteConsoleA(hCon, buf, (DWORD)strlen(buf), &wrote, nullptr);
            WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
        }
        return false;
    }

    CreateRenderTarget();
    return true;
}

void MainWindow::CleanupD3D11()
{
    CleanupRenderTarget();
    if (m_swapChain)  { m_swapChain->Release();  m_swapChain  = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice)  { m_d3dDevice->Release();  m_d3dDevice  = nullptr; }
}

void MainWindow::CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }
}

void MainWindow::CleanupRenderTarget()
{
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
}

// ============================================================================
// Window creation
// ============================================================================

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow)
{
    m_hInst = hInstance;

    const wchar_t CLASS_NAME[] = L"RTXVSRWindow";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    VSRConfig& cfg = m_config.Get();

    int winX = cfg.windowX;
    int winY = cfg.windowY;
    int winW = cfg.windowW;
    int winH = cfg.windowH;

    // Always center on screen
    winX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    winY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    m_hWnd = CreateWindowExW(WS_EX_APPWINDOW, CLASS_NAME, L"RTX 视频超分辨率工具",
                             WS_POPUP,
                             winX, winY, winW, winH,
                             nullptr, nullptr, hInstance, this);
    if (!m_hWnd)
        return false;

    if (!InitD3D11())
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Load Chinese-capable font with fallback paths
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 16.0f;
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\yahei.ttf",
    };
    ImFont* font = nullptr;
    for (auto* fp : fontPaths) {
        font = io.Fonts->AddFontFromFileTTF(
            fp, 16.0f, &fontCfg,
            io.Fonts->GetGlyphRangesChineseFull());
        if (font) break;
    }
    if (!font)
        io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(m_hWnd);
    ImGui_ImplDX11_Init(m_d3dDevice, m_d3dContext);

    LoadConfigToUI();

    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);
    return true;
}

// ============================================================================
// Message handling
// ============================================================================

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    MainWindow* pThis;
    if (msg == WM_CREATE) {
        pThis = (MainWindow*)((CREATESTRUCT*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (MainWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    }

    if (pThis)
        return pThis->OnMessage(msg, wParam, lParam);
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT MainWindow::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_SIZE:
        if (m_d3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            m_swapChain->ResizeBuffers(0,
                (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // Drag window via custom title bar
    case WM_NCHITTEST: {
        POINT pt;
        pt.x = (short)LOWORD(lParam);
        pt.y = (short)HIWORD(lParam);
        ScreenToClient(m_hWnd, &pt);
        RECT rc;
        GetClientRect(m_hWnd, &rc);
        const int titleBarH = 32;
        // Title bar area, but not the close button zone (rightmost ~30px)
        if (pt.y >= 0 && pt.y < titleBarH && pt.x < rc.right - 56)
            return HTCAPTION;
        return HTCLIENT;
    }

    // --- Pipeline progress (posted from worker thread) ---
    case WM_USER + 1: {
        auto* p = (PipelineProgress*)wParam;
        if (p) {
            m_progressPct = p->totalFrames > 0
                ? (p->currentFrame * 100.0f / p->totalFrames) : 0.0f;

            {
                std::lock_guard<std::mutex> lk(m_progressMutex);
                m_displayFps    = p->fps;
                m_displayEta    = p->etaSeconds;
                m_currentFrame  = p->currentFrame;
                m_totalFrames   = p->totalFrames;
            }

            snprintf(m_statusText, sizeof(m_statusText),
                     "帧 %d/%d  |  FPS: %.1f  |  剩余: %.0fs",
                     p->currentFrame, p->totalFrames, p->fps, p->etaSeconds);
            delete p;
        }
        return 0;
    }

    // --- Pipeline error (posted from worker thread) ---
    case WM_USER + 2: {
        auto* errMsg = (wchar_t*)wParam;
        if (errMsg) {
            narrow(errMsg, m_statusText, sizeof(m_statusText));
            delete[] errMsg;
        }
        m_isRunning = false;
        m_isPaused  = false;
        return 0;
    }

    // --- Pipeline completed (posted from worker thread) ---
    case WM_USER + 3: {
        auto elapsed = std::chrono::steady_clock::now() - m_startTime;
        float totalSec = std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count();

        int frames = 0;
        float fps = 0.0f;
        {
            std::lock_guard<std::mutex> lk(m_progressMutex);
            frames = m_totalFrames;
        }
        if (totalSec > 0.0f && frames > 0)
            fps = frames / totalSec;

        char timeStr[32];
        if (totalSec >= 60.0f) {
            int mins = (int)totalSec / 60;
            int secs = (int)totalSec % 60;
            snprintf(timeStr, sizeof(timeStr), "%d分%d秒", mins, secs);
        } else {
            snprintf(timeStr, sizeof(timeStr), "%.0f秒", totalSec);
        }

        snprintf(m_statusText, sizeof(m_statusText),
                 "完成: %d帧 | %s | %.1f fps", frames, timeStr, fps);

        snprintf(m_completeStats, sizeof(m_completeStats),
                 "总帧数: %d\n处理时间: %s\n平均速度: %.1f fps",
                 frames, timeStr, fps);

        m_isRunning          = false;
        m_isPaused           = false;
        m_showCompletePopup  = true;
        return 0;
    }
    }

    return DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

// ============================================================================
// Per-frame render
// ============================================================================

void MainWindow::Render()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderUI();

    ImGui::Render();

    const float clearColor[4] = { 0.12f, 0.12f, 0.13f, 1.00f };
    m_d3dContext->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_d3dContext->ClearRenderTargetView(m_rtv, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_swapChain->Present(1, 0);
}

// ============================================================================
// UI layout (all ImGui widgets)
// ============================================================================

void MainWindow::RenderUI()
{
    const float titleBarH = 32.0f;
    float winW = ImGui::GetIO().DisplaySize.x;
    float winH = ImGui::GetIO().DisplaySize.y;

    // --- Custom Title Bar ---
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(winW, titleBarH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 1.00f));
        ImGui::Begin("##titlebar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        // Title text
        ImGui::SetCursorPos(ImVec2(10, 6));
        ImGui::Text("RTX 视频超分辨率工具");

        // Minimize button
        ImGui::SameLine(winW - 56);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        if (ImGui::Button("-", ImVec2(24, 24))) {
            ShowWindow(m_hWnd, SW_MINIMIZE);
        }
        ImGui::PopStyleColor(3);

        // Close button
        ImGui::SameLine(winW - 28);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.0f, 0.0f, 1.0f));
        if (ImGui::Button("x", ImVec2(24, 24))) {
            DestroyWindow(m_hWnd);
        }
        ImGui::PopStyleColor(3);

        ImGui::End();
    }

    // --- Separator line between title bar and content ---
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddLine(ImVec2(0, titleBarH), ImVec2(winW, titleBarH), IM_COL32(60, 60, 70, 255));
    }

    // --- Main content ---
    ImGui::SetNextWindowPos(ImVec2(0, titleBarH));
    ImGui::SetNextWindowSize(ImVec2(winW, winH - titleBarH));
    ImGui::Begin("Main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // --- Input file ---
    ImGui::Text("输入文件");
    ImGui::SameLine(80);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("##input", m_inputPath, sizeof(m_inputPath),
        m_isRunning ? ImGuiInputTextFlags_ReadOnly : 0);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("浏览...##in") && !m_isRunning)
        OnSelectInput();

    // --- Video info ---
    if (m_inputInfo[0])
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", m_inputInfo);

    ImGui::Spacing();

    // --- Output file ---
    ImGui::Text("输出文件");
    ImGui::SameLine(80);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("##output", m_outputPath, sizeof(m_outputPath),
        m_isRunning ? ImGuiInputTextFlags_ReadOnly : 0);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("浏览...##out") && !m_isRunning)
        OnSelectOutput();

    ImGui::Separator();

    // --- AI Processing ---
    ImGui::Text("AI 处理");

    static const char* qualityNames[] = { "双三次", "低质量", "中等", "高质量", "极致" };
    for (int i = 0; i < 5; i++) {
        if (i > 0) ImGui::SameLine();
        if (m_isRunning) ImGui::BeginDisabled();
        if (ImGui::RadioButton(qualityNames[i], &m_qualityLevel, i)) {
            m_config.Get().qualityLevel = i;
        }
        if (m_isRunning) ImGui::EndDisabled();
    }

    ImGui::Spacing();

    // --- Output Size ---
    ImGui::Text("输出尺寸");
    ImGui::SameLine(80);
    ImGui::PushItemWidth(90);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* outputModes[] = { "2倍", "4倍", "自定义" };
    if (ImGui::Combo("##outmode", &m_outputMode, outputModes, 3))
        m_config.Get().outputMode = m_outputMode;

    if (m_outputMode == 2) {
        ImGui::SameLine();
        ImGui::Text("宽");
        ImGui::SameLine();
        ImGui::PushItemWidth(60);
        ImGui::InputInt("##w", &m_outputWidth);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Text("高");
        ImGui::SameLine();
        ImGui::PushItemWidth(60);
        ImGui::InputInt("##h", &m_outputHeight);
        ImGui::PopItemWidth();
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // --- Encoder ---
    ImGui::Text("编码器");
    ImGui::SameLine(80);
    ImGui::PushItemWidth(110);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* encoderNames[] = {
        "H.264 NVENC", "HEVC NVENC", "AV1 NVENC",
        "libx264", "libx265", "libaom-av1"
    };
    if (ImGui::Combo("##enc", &m_encoderIndex, encoderNames, 6))
        m_config.Get().encoderIndex = m_encoderIndex;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Text("CRF");
    ImGui::SameLine();
    ImGui::PushItemWidth(150);
    if (m_isRunning) ImGui::BeginDisabled();
    if (ImGui::SliderInt("##crf", &m_crf, 0, 51))
        m_config.Get().crf = m_crf;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Text("%d", m_crf);

    // --- Speed / GPU ---
    ImGui::Text("速度  ");
    ImGui::SameLine(80);
    ImGui::PushItemWidth(90);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* speedNames[] = { "最快", "快速", "中等", "慢速", "最慢" };
    if (ImGui::Combo("##speed", &m_encoderSpeed, speedNames, 5))
        m_config.Get().encoderSpeed = m_encoderSpeed;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Text("GPU");
    ImGui::SameLine();
    ImGui::PushItemWidth(210);
    if (m_isRunning) ImGui::BeginDisabled();
    {
        // Build dynamic GPU name list for ImGui
        EnumGpus();
        std::vector<const char*> gpuPtrs;
        for (const auto& n : m_gpuNames)
            gpuPtrs.push_back(n.c_str());
        if (m_gpuIndex >= (int)gpuPtrs.size())
            m_gpuIndex = 0;
        if (ImGui::Combo("##gpu", &m_gpuIndex, gpuPtrs.data(), (int)gpuPtrs.size()))
            m_config.Get().gpuIndex = m_gpuIndex;
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Text("封装");
    ImGui::SameLine();
    ImGui::PushItemWidth(60);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* containerNames[] = { "MP4", "MKV", "MOV" };
    if (ImGui::Combo("##container", &m_containerFormat, containerNames, 3))
        m_config.Get().containerFormat = m_containerFormat;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::Separator();

    // --- Progress ---
    ImGui::ProgressBar(m_progressPct / 100.0f, ImVec2(-1, 0), "");
    ImGui::Text("%s", m_statusText[0] ? m_statusText : "就绪");

    ImGui::Spacing();

    // --- Buttons ---
    float btnWidth = 100.0f;
    float avail    = ImGui::GetContentRegionAvail().x;
    float offset   = (avail - btnWidth * 2 - 20) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

    if (!m_isRunning) {
        if (ImGui::Button("开始", ImVec2(btnWidth, 0)))
            OnStartStop();
    } else {
        if (ImGui::Button("停止", ImVec2(btnWidth, 0)))
            OnStartStop();
    }

    ImGui::SameLine();
    if (m_isPaused) {
        if (ImGui::Button("继续", ImVec2(btnWidth, 0)))
            OnPauseResume();
    } else {
        if (m_isRunning) {
            if (ImGui::Button("暂停", ImVec2(btnWidth, 0)))
                OnPauseResume();
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("暂停", ImVec2(btnWidth, 0));
            ImGui::EndDisabled();
        }
    }

    // --- Completion popup ---
    if (m_showCompletePopup) {
        ImGui::OpenPopup("完成");
        m_showCompletePopup = false;
    }

    if (ImGui::BeginPopupModal("完成", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("视频处理完成！");
        ImGui::Separator();
        ImGui::Text("%s", m_completeStats);
        ImGui::Spacing();
        if (ImGui::Button("确定", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ============================================================================
// File selection
// ============================================================================

void MainWindow::OnSelectInput()
{
    wchar_t path[MAX_PATH] = {};
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';

    OPENFILENAMEW ofn = { sizeof(ofn), m_hWnd, m_hInst };
    ofn.lpstrInitialDir = exeDir;
    ofn.lpstrFilter = L"视频文件\0*.mp4;*.mov;*.avi;*.mkv;*.webm;*.flv;*.wmv\0"
                      L"所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile  = MAX_PATH;
    ofn.Flags     = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn)) {
        narrow(path, m_inputPath, sizeof(m_inputPath));
        wcscpy(m_config.Get().lastInputPath, path);

        VideoDecoder decoder;
        if (decoder.Open(path, &m_videoInfo)) {
            snprintf(m_inputInfo, sizeof(m_inputInfo),
                     "%d x %d  %.2f fps  %s",
                     m_videoInfo.width, m_videoInfo.height,
                     m_videoInfo.fps,
                     m_videoInfo.hasAudio ? "有音频" : "无音频");
            decoder.Close();
        } else {
            snprintf(m_inputInfo, sizeof(m_inputInfo), "无法打开文件");
        }

        // Auto-generate output path
        wchar_t outPath[MAX_PATH];
        wcscpy(outPath, path);
        wchar_t* dot = wcsrchr(outPath, L'.');
        if (dot) {
            static const wchar_t* containerExt[] = { L"_VSR.mp4", L"_VSR.mkv", L"_VSR.mov" };
            int idx = m_containerFormat;
            if (idx < 0) idx = 0;
            if (idx > 2) idx = 0;
            wcscpy(dot, containerExt[idx]);
        }
        narrow(outPath, m_outputPath, sizeof(m_outputPath));
        wcscpy(m_config.Get().lastOutputPath, outPath);
    }
}

void MainWindow::OnSelectOutput()
{
    wchar_t path[MAX_PATH] = {};
    wchar_t initPath[MAX_PATH] = {};
    widen(m_outputPath, initPath, MAX_PATH);

    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';

    OPENFILENAMEW ofn = { sizeof(ofn), m_hWnd, m_hInst };
    ofn.lpstrInitialDir = exeDir;
    ofn.lpstrFilter = L"MP4\0*.mp4\0MKV\0*.mkv\0MOV\0*.mov\0"
                      L"所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile  = MAX_PATH;
    ofn.Flags     = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;

    if (m_outputPath[0])
        wcscpy(path, initPath);

    if (GetSaveFileNameW(&ofn)) {
        narrow(path, m_outputPath, sizeof(m_outputPath));
        wcscpy(m_config.Get().lastOutputPath, path);
    }
}

// ============================================================================
// Start / Stop
// ============================================================================

void MainWindow::OnStartStop()
{
    if (!m_isRunning) {
        VSRConfig& cfg = m_config.Get();

        PipelineConfig pc;
        wchar_t wInput[MAX_PATH]  = {};
        wchar_t wOutput[MAX_PATH] = {};
        widen(m_inputPath,  wInput,  MAX_PATH);
        widen(m_outputPath, wOutput, MAX_PATH);
        pc.inputPath  = wInput;
        pc.outputPath = wOutput;
        pc.outputMode   = m_outputMode;
        pc.outputWidth  = m_outputWidth;
        pc.outputHeight = m_outputHeight;
        pc.qualityLevel = m_qualityLevel;
        pc.encoderIndex = m_encoderIndex;
        pc.crf          = m_crf;
        pc.encoderSpeed = m_encoderSpeed;
        pc.gpuIndex     = m_gpuIndex;
        pc.container    = m_containerFormat;
        pc.outputFps    = 0;

        cfg.qualityLevel = m_qualityLevel;
        cfg.outputMode   = m_outputMode;
        cfg.encoderIndex = m_encoderIndex;
        cfg.crf          = m_crf;
        cfg.encoderSpeed = m_encoderSpeed;
        cfg.gpuIndex     = m_gpuIndex;

        m_pipeline.Stop();
        if (m_pipeline.Start(pc)) {
            m_isRunning          = true;
            m_progressPct        = 0.0f;
            m_showCompletePopup  = false;
            m_startTime          = std::chrono::steady_clock::now();
            snprintf(m_statusText, sizeof(m_statusText), "处理中...");
        } else {
            MessageBoxW(m_hWnd, L"管道已在运行中", L"错误", MB_ICONERROR);
        }
    } else {
        m_pipeline.Stop();
        m_isRunning = false;
        m_isPaused  = false;
        snprintf(m_statusText, sizeof(m_statusText), "已停止");
        m_progressPct = 0.0f;
    }
}

// ============================================================================
// Pause / Resume
// ============================================================================

void MainWindow::OnPauseResume()
{
    if (m_isPaused) {
        m_pipeline.Resume();
        m_isPaused = false;
    } else {
        m_pipeline.Pause();
        m_isPaused = true;
    }
}

// ============================================================================
// Pipeline callbacks
// ============================================================================

void MainWindow::OnPipelineProgress(const PipelineProgress& p)
{
    PostMessageW(m_hWnd, WM_USER + 1, (WPARAM)new PipelineProgress(p), 0);
}

void MainWindow::OnPipelineError(const wchar_t* msg)
{
    size_t len = wcslen(msg) + 1;
    wchar_t* copy = new wchar_t[len];
    wcscpy(copy, msg);
    PostMessageW(m_hWnd, WM_USER + 2, (WPARAM)copy, 0);
}

void MainWindow::OnPipelineCompleted()
{
    PostMessageW(m_hWnd, WM_USER + 3, 0, 0);
}

// ============================================================================
// GPU enumeration
// ============================================================================

void MainWindow::EnumGpus()
{
    m_gpuNames.clear();
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess || count <= 0) {
        m_gpuNames.push_back("GPU 0 (default)");
        return;
    }
    for (int i = 0; i < count; i++) {
        cudaDeviceProp prop;
        if (cudaGetDeviceProperties(&prop, i) == cudaSuccess) {
            char buf[128];
            snprintf(buf, sizeof(buf), "GPU %d: %s (%d MB)",
                     i, prop.name, (int)(prop.totalGlobalMem / (1024 * 1024)));
            m_gpuNames.push_back(buf);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "GPU %d", i);
            m_gpuNames.push_back(buf);
        }
    }
}

// ============================================================================
// Config UI synchronisation
// ============================================================================

void MainWindow::LoadConfigToUI()
{
    VSRConfig& cfg = m_config.Get();

    narrow(cfg.lastInputPath,  m_inputPath,  sizeof(m_inputPath));
    narrow(cfg.lastOutputPath, m_outputPath, sizeof(m_outputPath));
    m_qualityLevel = cfg.qualityLevel;
    m_outputMode   = cfg.outputMode;
    m_outputWidth  = cfg.fixedWidth;
    m_outputHeight = cfg.fixedHeight;
    m_encoderIndex = cfg.encoderIndex;
    m_crf          = cfg.crf;
    m_encoderSpeed = cfg.encoderSpeed;
    m_gpuIndex     = cfg.gpuIndex;
    m_containerFormat = cfg.containerFormat;
}

void MainWindow::SaveUIToConfig()
{
    VSRConfig& cfg = m_config.Get();

    RECT rc;
    GetWindowRect(m_hWnd, &rc);
    if (rc.left != CW_USEDEFAULT) {
        cfg.windowX = rc.left;
        cfg.windowY = rc.top;
    }

    widen(m_inputPath,  cfg.lastInputPath,  MAX_PATH);
    widen(m_outputPath, cfg.lastOutputPath, MAX_PATH);

    cfg.qualityLevel = m_qualityLevel;
    cfg.outputMode   = m_outputMode;
    cfg.fixedWidth   = m_outputWidth;
    cfg.fixedHeight  = m_outputHeight;
    cfg.encoderIndex = m_encoderIndex;
    cfg.crf          = m_crf;
    cfg.encoderSpeed = m_encoderSpeed;
    cfg.gpuIndex     = m_gpuIndex;
    cfg.containerFormat = m_containerFormat;
}
