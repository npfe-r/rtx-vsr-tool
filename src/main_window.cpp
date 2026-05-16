#include "main_window.h"
#include <commdlg.h>
#include <shellapi.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cuda_runtime.h>
#include "debug_util.h"

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
    m_pipeline.onStatus    = [this](const char* msg)           { OnPipelineStatus(msg); };
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
    sd.Flags      = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hWnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed  = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

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

        char logPath[MAX_PATH];
        GetModuleFileNameA(NULL, logPath, sizeof(logPath));
        char* slash = strrchr(logPath, '\\');
        if (slash) *(slash + 1) = '\0';
        strcat_s(logPath, "pipeline_debug.log");
        FILE* f = nullptr;
        fopen_s(&f, logPath, "a");
        if (f) { fprintf(f, "%s\n", buf); fflush(f); fclose(f); }

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
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        LogMsg("UI: ","CreateRenderTarget: GetBuffer failed");
        return;
    }
    hr = m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    if (FAILED(hr)) {
        LogMsg("UI: ","CreateRenderTarget: CreateRenderTargetView failed");
        m_rtv = nullptr;
    }
    backBuffer->Release();
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

    int winW = cfg.windowW;
    int winH = cfg.windowH;

    // Always center on screen
    int winX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int winY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

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

    // Try to read last input file info on startup — skip silently on failure
    if (m_inputPath[0]) {
        wchar_t wpath[MAX_PATH];
        widen(m_inputPath, wpath, MAX_PATH);
        VideoDecoder decoder;
        if (decoder.Open(wpath, &m_videoInfo)) {
            snprintf(m_inputInfo, sizeof(m_inputInfo),
                     "%d x %d  %.2f fps  %s",
                     m_videoInfo.width, m_videoInfo.height,
                     m_videoInfo.fps,
                     m_videoInfo.hasAudio ? "有音频" : "无音频");
            decoder.Close();
        }
    }

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
            HRESULT hr = m_swapChain->ResizeBuffers(0,
                (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) {
                LogMsg("UI: ","ResizeBuffers failed");
            }
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
                // EMA smoothing on per-frame ms to reduce FPS/ETA jitter
                float rawMs = p->fps > 0.001f ? 1000.0f / p->fps : 16.0f;
                if (m_smoothedMs < 0.001f) {
                    m_smoothedMs = rawMs;
                } else {
                    const float alpha = 0.12f;
                    m_smoothedMs = alpha * rawMs + (1.0f - alpha) * m_smoothedMs;
                }
                int remaining = p->totalFrames - p->currentFrame;
                m_displayFps = 1000.0f / m_smoothedMs;
                m_displayEta = remaining * m_smoothedMs / 1000.0f;

                std::lock_guard<std::mutex> lk(m_progressMutex);
                m_currentFrame  = p->currentFrame;
                m_totalFrames   = p->totalFrames;
            }

            snprintf(m_statusText, sizeof(m_statusText),
                     "帧 %d/%d  |  FPS: %.1f  |  剩余: %.0fs",
                     p->currentFrame, p->totalFrames, m_displayFps, m_displayEta);
            strncpy(m_decodeMode, p->decodeMode, sizeof(m_decodeMode) - 1);
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
        m_hasError  = true;
        return 0;
    }

    // --- Pipeline status message (posted from worker thread) ---
    case WM_USER + 4: {
        auto* statusMsg = (char*)wParam;
        if (statusMsg) {
            if (!m_hasError)
                snprintf(m_statusText, sizeof(m_statusText), "%s", statusMsg);
            delete[] statusMsg;
        }
        return 0;
    }

    // --- Auto-start (posted from AutoStart()) ---
    case WM_USER + 10:
        if (!m_isRunning) OnStartStop();
        return 0;

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
    const float leftPanelW = 185.0f;
    float winW = ImGui::GetIO().DisplaySize.x;
    float winH = ImGui::GetIO().DisplaySize.y;

    // ===== Title Bar =====
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

        ImGui::SetCursorPos(ImVec2(10, 6));
        ImGui::Text("RTX 视频超分辨率工具");

        ImGui::SameLine(winW - 56);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        if (ImGui::Button("-", ImVec2(24, 24)))
            ShowWindow(m_hWnd, SW_MINIMIZE);
        ImGui::PopStyleColor(3);

        ImGui::SameLine(winW - 28);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.0f, 0.0f, 1.0f));
        if (ImGui::Button("x", ImVec2(24, 24)))
            DestroyWindow(m_hWnd);
        ImGui::PopStyleColor(3);

        ImGui::End();
    }

    float contentY = titleBarH;
    float contentH = winH - contentY;

    // ===== Background separators =====
    // Horizontal separator drawn between top-bar area and mid section will be
    // drawn by the top-bar window itself.  Here we draw the vertical divider
    // between the left info panel and right settings panel.
    // The bottom horizontal separator is handled similarly.
    // We draw all background lines here for simplicity.

    // ===== Top Bar — File Selection =====
    const float topBarH = 64.0f; // 2 rows of controls
    {
        ImGui::SetNextWindowPos(ImVec2(0, contentY));
        ImGui::SetNextWindowSize(ImVec2(winW, topBarH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.09f, 0.10f, 1.00f));
        ImGui::Begin("##topbar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        const float tbLabelW = 44.0f;
        const float btnBrowseW = 58.0f;
        ImGui::SetCursorPos(ImVec2(10, 8));

        // Input file row
        ImGui::Text("输入");
        ImGui::SameLine(tbLabelW);
        ImGui::PushItemWidth(winW - tbLabelW - btnBrowseW - 24);

        ImGui::InputText("##inpath", m_inputPath, sizeof(m_inputPath),
            m_isRunning ? ImGuiInputTextFlags_ReadOnly : 0);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("浏览##in", ImVec2(btnBrowseW, 0)) && !m_isRunning)
            OnSelectInput();

        // Output file row
        ImGui::SetCursorPos(ImVec2(10, 34));
        ImGui::Text("输出");
        ImGui::SameLine(tbLabelW);
        ImGui::PushItemWidth(winW - tbLabelW - btnBrowseW - 24);
        ImGui::InputText("##outpath", m_outputPath, sizeof(m_outputPath),
            m_isRunning ? ImGuiInputTextFlags_ReadOnly : 0);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("浏览##out", ImVec2(btnBrowseW, 0)) && !m_isRunning)
            OnSelectOutput();

        ImGui::End();
    }

    // ===== Mid Section — Info (left) + Settings (right) =====
    const float midY = contentY + topBarH;

    // Calculate mid section height from settings panel content (no scrollbar)
    // Count of control rows (each uses AlignTextToFramePadding + Text + SameLine + widget)
    // Rows: GPU, Quality, HDR, OutputSize, Resolution, FPS, Encoder, CRF, Speed, Container, AudioMode, AudioBitrate = 12
    // Separators between groups: after HDR, after FPS, after Container = 3
    float midH;
    {
        float itemH = ImGui::GetFrameHeightWithSpacing();
        float sepH  = ImGui::GetStyle().ItemSpacing.y;
        float padY  = ImGui::GetStyle().WindowPadding.y;
        const int nControls   = 12;  // update when adding/removing setting rows
        const int nSeparators = 3;
        midH = nControls * itemH + nSeparators * sepH + padY * 2.0f;
    }

    // Background vertical divider
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddLine(ImVec2(leftPanelW, midY), ImVec2(leftPanelW, midY + midH),
            IM_COL32(60, 60, 70, 255));
    }

    // ---- Left Panel ----
    ImGui::SetNextWindowPos(ImVec2(0, midY));
    ImGui::SetNextWindowSize(ImVec2(leftPanelW, midH));
    ImGui::Begin("##infopanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Input info — top half
    float halfH = ImGui::GetContentRegionAvail().y * 0.48f;
    ImGui::BeginChild("##inputinfo", ImVec2(-1, halfH));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("输入视频");
    ImGui::Spacing();

    if (m_videoInfo.width > 0) {
        ImGui::Text("分辨率");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%d x %d",
            m_videoInfo.width, m_videoInfo.height);
        ImGui::Text("帧率");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%.2f fps", m_videoInfo.fps);
        if (m_videoInfo.videoCodecName[0]) {
            ImGui::Text("编码");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", m_videoInfo.videoCodecName);
        }
        ImGui::Text("帧数");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%d", m_videoInfo.totalFrames);

        // Input colour info
        {
            const char* trc  = "";
            if (m_videoInfo.avColorTransfer == 1) trc = "BT.709";
            else if (m_videoInfo.avColorTransfer == 16) trc = "PQ";
            else if (m_videoInfo.avColorTransfer == 18) trc = "HLG";
            else trc = "SDR";
            const char* range = (m_videoInfo.avColorRange == 2) ? "full" : "limited";
            const char* cs = "";
            if (m_videoInfo.avColorSpace == 1) cs = "BT.709";
            else if (m_videoInfo.avColorSpace == 9) cs = "BT.2020";
            else if (m_videoInfo.avColorSpace == 5 || m_videoInfo.avColorSpace == 6) cs = "BT.601";
            else cs = "未知";
            bool isHdr = (m_videoInfo.avColorTransfer == 16 || m_videoInfo.avColorTransfer == 18);
            char colorBuf[128];
            snprintf(colorBuf, sizeof(colorBuf), "%s  %s  %s", cs, trc, range);
            ImGui::Text("色彩");
            ImGui::SameLine();
            if (isHdr)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), "%s", colorBuf);
            else
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", colorBuf);
        }

        if (m_videoInfo.hasAudio) {
            char audioBuf[96];
            if (m_videoInfo.audioCodecName[0])
                snprintf(audioBuf, sizeof(audioBuf), "%s  %dHz %dch",
                    m_videoInfo.audioCodecName, m_videoInfo.audioSampleRate, m_videoInfo.audioChannels);
            else
                snprintf(audioBuf, sizeof(audioBuf), "%dHz %dch",
                    m_videoInfo.audioSampleRate, m_videoInfo.audioChannels);
            ImGui::Text("音频");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", audioBuf);
        } else {
            ImGui::Text("音频");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "无");
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "未选择文件");
    }

    ImGui::EndChild(); // ##inputinfo

    // Separator between input and output info
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x + 4, p.y), ImVec2(p.x + leftPanelW - 4, p.y),
            IM_COL32(60, 60, 70, 255));
    }
    ImGui::Spacing();

    // Output info — bottom half
    ImGui::BeginChild("##outputinfo", ImVec2(-1, halfH));

    ImGui::Text("输出视频");
    ImGui::Spacing();

    if (m_videoInfo.width > 0) {
        int srcW = m_videoInfo.width;
        int srcH = m_videoInfo.height;
        int outW, outH;
        if (m_outputMode == 0) { outW = srcW * 2; outH = srcH * 2; }
        else if (m_outputMode == 1) { outW = srcW * 4; outH = srcH * 4; }
        else { outW = m_outputWidth; outH = m_outputHeight; }
        outW = (outW + 15) & ~15;
        outH = (outH + 15) & ~15;

        ImGui::Text("分辨率");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%d x %d", outW, outH);

        // Output FPS
        double outFps = m_outputFps > 0 ? (double)m_outputFps : m_videoInfo.fps;
        ImGui::Text("帧率");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%.2f fps", outFps);

        static const char* encNames[] = {"H.264 NVENC", "HEVC NVENC", "AV1 NVENC",
                                         "libx264", "libx265", "libaom-av1", "SVT-AV1"};
        ImGui::Text("编码");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", encNames[m_encoderIndex]);

        // Estimate output frame count (same as source unless fps changes)
        int outFrames = m_videoInfo.totalFrames;
        ImGui::Text("帧数");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%d", outFrames);

        // Output colour info
        {
            bool outIsHdr = m_trueHdrEnabled;
            bool inIsHdr  = (m_videoInfo.avColorTransfer == 16 || m_videoInfo.avColorTransfer == 18);
            ImGui::Text("色彩");
            ImGui::SameLine();
            if (outIsHdr) {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "BT.2020 PQ  10-bit HDR");
            } else if (inIsHdr) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), "BT.709 gamma  8-bit SDR  (HDR注入)");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "BT.709 gamma  8-bit SDR");
            }
        }

        static const char* audNames[] = {"无", "复制", "AAC"};
        ImGui::Text("音频");
        ImGui::SameLine();
        if (m_audioMode == 0)
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "无");
        else if (m_audioMode == 2)
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "AAC %dkbps", m_audioBitrate);
        else
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "复制");

        // Encoder compatibility check — update warning for bottom bar
        {
            bool hadWarning = m_encoderWarning[0] != '\0';
            m_encoderWarning[0] = '\0';
            // TrueHDR + unsupported encoder warning
            if (m_trueHdrEnabled && (m_encoderIndex == 0 || m_encoderIndex == 3)) {
                snprintf(m_encoderWarning, sizeof(m_encoderWarning),
                         "TrueHDR 需要 10-bit 编码器，当前编码器不支持，建议切换到 HEVC 或 AV1");
            } else if (m_encoderIndex == 0) {
                if (outW > 4096 || outH > 4096)
                    snprintf(m_encoderWarning, sizeof(m_encoderWarning),
                             "H.264 NVENC 不支持 %dx%d (最大 4096x4096)，建议切换到 HEVC 或 AV1",
                             outW, outH);
            } else if (m_encoderIndex <= 2) {
                if (outW > 8192 || outH > 8192)
                    snprintf(m_encoderWarning, sizeof(m_encoderWarning),
                             "NVENC 不支持 %dx%d (最大 8192x8192)，建议切换到软件编码器",
                             outW, outH);
            }
            if (hadWarning && m_encoderWarning[0] == '\0' && !m_isRunning)
                m_statusText[0] = '\0';
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "选择文件后显示");
    }

    ImGui::EndChild(); // ##outputinfo

    ImGui::End(); // ##infopanel

    // ---- Right Panel — Settings ----
    const float labelW = 76.0f;
    const float btnBrowseW = 58.0f;
    const float rightX = leftPanelW + 1;
    const float rightW = winW - rightX;

    ImGui::SetNextWindowPos(ImVec2(rightX, midY));
    ImGui::SetNextWindowSize(ImVec2(rightW, midH));
    ImGui::Begin("##settings", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // GPU
    ImGui::AlignTextToFramePadding();
    ImGui::Text("GPU");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    {
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

    // Quality (Bicubic/qualityLevel=0 is intentionally excluded from the UI combo —
    // reserved for internal/script use only. The combo maps 1→Low .. 4→Ultra.)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("质量");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* qualityNames[] = { "低质量", "中等", "高质量", "极致" };
    int qualityIdx = m_qualityLevel - 1;
    if (qualityIdx < 0) qualityIdx = 0;
    if (qualityIdx > 3) qualityIdx = 3;
    if (ImGui::Combo("##quality", &qualityIdx, qualityNames, 4)) {
        m_qualityLevel = qualityIdx + 1;
        m_config.Get().qualityLevel = m_qualityLevel;
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // TrueHDR toggle + params button
    ImGui::AlignTextToFramePadding();
    ImGui::Text("HDR");
    ImGui::SameLine(labelW);
    if (m_isRunning) ImGui::BeginDisabled();
    {
        bool hdrOn = (m_trueHdrEnabled != 0);
        if (ImGui::Checkbox("##truehdr", &hdrOn)) {
            m_trueHdrEnabled = hdrOn ? 1 : 0;
            // Auto-switch to HEVC NVENC if current encoder doesn't support 10-bit
            if (m_trueHdrEnabled && (m_encoderIndex == 0 || m_encoderIndex == 3))
                m_encoderIndex = 1;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "TrueHDR");
        ImGui::SameLine();
        float btnW = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button("参数设置", ImVec2(btnW, 0))) {
            ImGui::OpenPopup("truehdr_params");
        }
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::Separator();

    // TrueHDR parameter popup
    ImGui::SetNextWindowSize(ImVec2(460, 176), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("TrueHDR 设置", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Spacing();

        auto thdrSlider = [&](const char* label, const char* id, int* val, int minV, int maxV) {
            ImGui::Text("%s", label);
            ImGui::SameLine(78.0f);
            float sliderW = ImGui::GetContentRegionAvail().x - 56.0f;
            ImGui::PushItemWidth(sliderW);
            ImGui::PushID(id);
            ImGui::SliderInt("##sl", val, minV, maxV, "%d");
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::PushItemWidth(46.0f);
            ImGui::InputInt("##in", val, 0, 0);
            ImGui::PopItemWidth();
            ImGui::PopID();
            if (*val < minV) *val = minV;
            if (*val > maxV) *val = maxV;
        };

        thdrSlider("对比度",   "c", &m_thdrContrast,    0,   200);
        thdrSlider("饱和度",   "s", &m_thdrSaturation,  0,   200);
        thdrSlider("中间灰",   "m", &m_thdrMiddleGray,  10,  100);
        thdrSlider("峰值亮度", "l", &m_thdrMaxLuminance, 400, 2000);

        ImGui::Spacing();
        ImGui::Separator();

        float btnW = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        if (ImGui::Button("关闭", ImVec2(btnW, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("恢复默认", ImVec2(btnW, 0))) {
            m_thdrContrast    = 100;
            m_thdrSaturation  = 100;
            m_thdrMiddleGray  = 50;
            m_thdrMaxLuminance = 1000;
        }

        ImGui::EndPopup();
    }

    // Output Size
    ImGui::AlignTextToFramePadding();
    ImGui::Text("尺寸");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* outputModes[] = { "2倍", "4倍", "自定义" };
    if (ImGui::Combo("##outmode", &m_outputMode, outputModes, 3))
        m_config.Get().outputMode = m_outputMode;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // Resolution
    ImGui::AlignTextToFramePadding();
    ImGui::Text("分辨率");
    ImGui::SameLine(labelW);
    bool disableRes = (m_outputMode != 2) || m_isRunning;
    if (disableRes) ImGui::BeginDisabled();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.44f);
    ImGui::InputInt("##outw", &m_outputWidth);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Text("x");
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputInt("##outh", &m_outputHeight);
    ImGui::PopItemWidth();
    if (disableRes) ImGui::EndDisabled();

    // Output FPS
    ImGui::AlignTextToFramePadding();
    ImGui::Text("FPS");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 75);
    if (m_isRunning) ImGui::BeginDisabled();
    if (ImGui::InputInt("##outfps", &m_outputFps, 0, 0)) {
        if (m_videoInfo.fps > 0 && m_outputFps > (int)m_videoInfo.fps)
            m_outputFps = (int)m_videoInfo.fps;
        m_config.Get().outputFps = m_outputFps;
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextDisabled("0=源帧率");
    ImGui::Separator();

    // Encoder
    ImGui::AlignTextToFramePadding();
    ImGui::Text("编码器");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* encoderNames[] = {
        "H.264 NVENC", "HEVC NVENC", "AV1 NVENC",
        "libx264", "libx265", "libaom-av1", "SVT-AV1"
    };
    if (ImGui::Combo("##enc", &m_encoderIndex, encoderNames, 7))
        m_config.Get().encoderIndex = m_encoderIndex;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // CRF
    ImGui::AlignTextToFramePadding();
    ImGui::Text("CRF");
    ImGui::SameLine(labelW);
    float crfSliderW = ImGui::GetContentRegionAvail().x - 56.0f;
    ImGui::PushItemWidth(crfSliderW);
    if (m_isRunning) ImGui::BeginDisabled();
    if (ImGui::SliderInt("##crf", &m_crf, 0, 51))
        m_config.Get().crf = m_crf;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(46);
    if (m_isRunning) ImGui::BeginDisabled();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(13, ImGui::GetStyle().FramePadding.y));
    if (ImGui::InputInt("##crfv", &m_crf, 0, 0)) {
        if (m_crf < 0) m_crf = 0;
        if (m_crf > 51) m_crf = 51;
        m_config.Get().crf = m_crf;
    }
    ImGui::PopStyleVar();
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // Speed
    ImGui::AlignTextToFramePadding();
    ImGui::Text("速度");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* speedNames[] = { "最快", "快速", "中等", "慢速", "最慢" };
    if (ImGui::Combo("##speed", &m_encoderSpeed, speedNames, 5))
        m_config.Get().encoderSpeed = m_encoderSpeed;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // Container
    ImGui::AlignTextToFramePadding();
    ImGui::Text("封装");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* containerNames[] = { "MP4", "MOV" };
    if (ImGui::Combo("##container", &m_containerFormat, containerNames, 2)) {
        m_config.Get().containerFormat = m_containerFormat;
        UpdateOutputExtension();
    }
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::Separator();

    // Audio
    ImGui::AlignTextToFramePadding();
    ImGui::Text("音频");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (m_isRunning) ImGui::BeginDisabled();
    static const char* audioModes[] = { "无音频", "复制源", "AAC编码" };
    if (ImGui::Combo("##audio", &m_audioMode, audioModes, 3))
        m_config.Get().audioMode = m_audioMode;
    if (m_isRunning) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    const bool audioBitrateDisabled = m_isRunning || (m_audioMode != 2);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("码率");
    ImGui::SameLine(labelW);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    if (audioBitrateDisabled) ImGui::BeginDisabled();
    static const char* bitrateNames[] = { "64k", "96k", "128k", "192k", "256k", "320k" };
    static const int   bitrateValues[] = { 64, 96, 128, 192, 256, 320 };
    int brIdx = 2;
    for (int i = 0; i < 6; i++) {
        if (m_audioBitrate == bitrateValues[i]) { brIdx = i; break; }
    }
    if (ImGui::Combo("##abr", &brIdx, bitrateNames, 6)) {
        m_audioBitrate = bitrateValues[brIdx];
        m_config.Get().audioBitrate = m_audioBitrate;
    }
    if (audioBitrateDisabled) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::End(); // ##settings

    // ===== Bottom Bar — Progress + Buttons =====
    const float bottomY = midY + midH;
    const float bottomBarH = (contentH - topBarH - midH < 85.0f) ? 85.0f : contentH - topBarH - midH;

    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddLine(ImVec2(0, bottomY), ImVec2(winW, bottomY), IM_COL32(60, 60, 70, 255));
    }

    ImGui::SetNextWindowPos(ImVec2(0, bottomY));
    ImGui::SetNextWindowSize(ImVec2(winW, bottomBarH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.09f, 0.10f, 1.00f));
    ImGui::Begin("##bottombar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(10, 6));
    ImGui::ProgressBar(m_progressPct / 100.0f, ImVec2(winW - 24, 0), "");

    {
        const char* status = m_statusText[0] ? m_statusText : "就绪";
        bool showWarning = m_encoderWarning[0] && !m_isRunning;
        if (showWarning) status = m_encoderWarning;
        float tw = ImGui::CalcTextSize(status).x;
        ImGui::SetCursorPos(ImVec2((winW - tw) * 0.5f, 32));
        if (showWarning)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "%s", status);
        else
            ImGui::Text("%s", status);

        if (m_decodeMode[0]) {
            char modeBuf[32];
            snprintf(modeBuf, sizeof(modeBuf), "[%s]", m_decodeMode);
            float mw = ImGui::CalcTextSize(modeBuf).x;
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(winW - mw - 14, 32));
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1), "%s", modeBuf);
        }
    }

    // Buttons
    float btnWidth = 100.0f;
    float btnY = ImGui::GetCursorPosY() + 4;
    float btnAreaW = winW - 20;
    float btnOffset = (btnAreaW - btnWidth * 2 - 16) * 0.5f;

    ImGui::SetCursorPos(ImVec2(10 + btnOffset, btnY));

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

    if (ImGui::BeginPopupModal("完成", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("%s", m_completeStats);
        ImGui::Spacing();
        float btnW = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button("打开输出文件夹", ImVec2(btnW, 0))) {
            wchar_t outPathW[512];
            widen(m_outputPath, outPathW, 512);
            // Construct explorer.exe /select,"path" to open folder and select the file
            wchar_t cmd[1024];
            swprintf_s(cmd, L"/select,\"%s\"", outPathW);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", cmd, nullptr, SW_SHOW);
        }
        if (ImGui::Button("确定", ImVec2(btnW, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End(); // ##bottombar

    // ===== Auto-size window on first frame =====
    if (!m_autoSized) {
        int neededH = (int)(titleBarH + topBarH + midH + 85.0f + 2);
        RECT rc;
        GetWindowRect(m_hWnd, &rc);
        int curH = rc.bottom - rc.top;
        if (neededH != curH) {
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenW - (int)winW) / 2;
            int y = (screenH - neededH) / 2;
            SetWindowPos(m_hWnd, nullptr, x, y, (int)winW, neededH, SWP_NOZORDER);
        }
        m_autoSized = true;
    }
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
        m_nvdecProbed = false;
        m_fallbackMsg[0] = '\0';

        VideoDecoder decoder;
        if (decoder.Open(path, &m_videoInfo)) {
            snprintf(m_inputInfo, sizeof(m_inputInfo),
                     "%d x %d  %.2f fps  %s",
                     m_videoInfo.width, m_videoInfo.height,
                     m_videoInfo.fps,
                     m_videoInfo.hasAudio ? "有音频" : "无音频");
            decoder.Close();

            // Probe NVDEC GPU decode availability
            VideoDecoder gpuProbe;
            VideoInfo gpuInfo;
            if (gpuProbe.Open(path, &gpuInfo, true)) {
                bool hwAvail = gpuProbe.IsHWDecoding();
                gpuProbe.Close();
                m_gpuDecodeAvailable = hwAvail;
                m_nvdecProbed = true;
                if (!hwAvail) {
                    snprintf(m_fallbackMsg, sizeof(m_fallbackMsg),
                             "NVDEC 硬件解码不可用，将使用 CPU 软件解码。\n"
                             "CPU 解码会占用大量内存带宽，处理速度可能显著降低。");
                }
            } else {
                // GPU probe failed — maybe codec unsupported by NVDEC
                m_gpuDecodeAvailable = false;
                m_nvdecProbed = true;
                snprintf(m_fallbackMsg, sizeof(m_fallbackMsg),
                         "GPU 解码初始化失败，将使用 CPU 软件解码。\n"
                         "处理速度可能显著降低。");
            }

            if (m_videoInfo.fps > 0 && m_outputFps > (int)m_videoInfo.fps) {
                m_outputFps = (int)m_videoInfo.fps;
                m_config.Get().outputFps = m_outputFps;
            }
        } else {
            snprintf(m_inputInfo, sizeof(m_inputInfo), "无法打开文件");
            snprintf(m_statusText, sizeof(m_statusText), "无法打开输入文件");
        }

        // Auto-generate output path
        wchar_t outPath[MAX_PATH];
        wcscpy(outPath, path);
        wchar_t* dot = wcsrchr(outPath, L'.');
        if (dot) {
            static const wchar_t* containerExt[] = { L"_VSR.mp4", L"_VSR.mov" };
            int idx = m_containerFormat;
            if (idx < 0) idx = 0;
            if (idx > 1) idx = 0;
            wcscpy(dot, containerExt[idx]);
        }
        narrow(outPath, m_outputPath, sizeof(m_outputPath));
        wcscpy(m_config.Get().lastOutputPath, outPath);
    }
}

void MainWindow::UpdateOutputExtension()
{
    wchar_t outPath[MAX_PATH];
    widen(m_outputPath, outPath, MAX_PATH);
    wchar_t* dot = wcsrchr(outPath, L'.');
    if (dot) {
        static const wchar_t* containerExt[] = { L"_VSR.mp4", L"_VSR.mov" };
        int idx = m_containerFormat;
        if (idx < 0 || idx > 1) idx = 0;
        wcscpy(dot, containerExt[idx]);
    }
    narrow(outPath, m_outputPath, sizeof(m_outputPath));
}

void MainWindow::OnSelectOutput()
{
    UpdateOutputExtension();

    wchar_t path[MAX_PATH] = {};
    wchar_t initPath[MAX_PATH] = {};
    widen(m_outputPath, initPath, MAX_PATH);

    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';

    OPENFILENAMEW ofn = { sizeof(ofn), m_hWnd, m_hInst };
    ofn.lpstrInitialDir = exeDir;
    ofn.lpstrFilter = L"MP4\0*.mp4\0MOV\0*.mov\0"
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

bool MainWindow::AutoStart()
{
    if (m_isRunning) return false;
    PostMessageW(m_hWnd, WM_USER + 10, 0, 0);
    return true;
}

void MainWindow::OnStartStop()
{
    if (!m_isRunning) {
        // ---- Pre-start validation ----
        if (!m_inputPath[0]) {
            MessageBoxW(m_hWnd, L"请先选择输入视频文件。", L"输入未设置", MB_ICONWARNING);
            return;
        }
        if (!m_outputPath[0]) {
            MessageBoxW(m_hWnd, L"请先设置输出文件路径。", L"输出未设置", MB_ICONWARNING);
            return;
        }
        // Check input file exists
        {
            wchar_t wpath[MAX_PATH];
            widen(m_inputPath, wpath, MAX_PATH);
            if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
                wchar_t msg[MAX_PATH + 64];
                swprintf(msg, L"输入文件不存在:\n%s\n\n请重新选择文件。", wpath);
                MessageBoxW(m_hWnd, msg, L"文件未找到", MB_ICONERROR);
                return;
            }
        }

        // Fallback warning: NVDEC unavailable
        if (m_nvdecProbed && !m_gpuDecodeAvailable && m_fallbackMsg[0]) {
            int ret = MessageBoxW(m_hWnd,
                L"NVDEC 硬件解码不可用，将回退到 CPU 软件解码。\n\n"
                L"CPU 解码会占用大量内存带宽，处理速度可能显著降低。\n"
                L"对于高分辨率或高码率视频可能导致无法实时处理。\n\n"
                L"是否继续？",
                L"硬件解码回退警告",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (ret != IDYES) {
                snprintf(m_statusText, sizeof(m_statusText), "用户取消");
                return;
            }
        }

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
        pc.audioMode = m_audioMode;
        pc.audioBitrate = m_audioBitrate;
        pc.outputFps    = m_outputFps;
        pc.trueHdrEnabled = (m_trueHdrEnabled != 0);
        pc.thdrContrast    = m_thdrContrast;
        pc.thdrSaturation  = m_thdrSaturation;
        pc.thdrMiddleGray  = m_thdrMiddleGray;
        pc.thdrMaxLuminance = m_thdrMaxLuminance;

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
            m_hasError           = false;
            m_startTime          = std::chrono::steady_clock::now();
            m_smoothedMs         = 0.0f;
            snprintf(m_statusText, sizeof(m_statusText), "处理中...");
        } else {
            MessageBoxW(m_hWnd, L"管道已在运行中", L"错误", MB_ICONERROR);
        }
    } else {
        m_pipeline.Stop();
        m_isRunning = false;
        m_isPaused  = false;
        m_hasError  = false;
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

void MainWindow::OnPipelineStatus(const char* msg)
{
    size_t len = strlen(msg) + 1;
    char* copy = new char[len];
    memcpy(copy, msg, len);
    PostMessageW(m_hWnd, WM_USER + 4, (WPARAM)copy, 0);
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
    if (m_qualityLevel < 1) m_qualityLevel = 1;
    m_outputMode   = cfg.outputMode;
    m_outputWidth  = cfg.fixedWidth;
    m_outputHeight = cfg.fixedHeight;
    m_encoderIndex = cfg.encoderIndex;
    m_crf          = cfg.crf;
    m_encoderSpeed = cfg.encoderSpeed;
    m_gpuIndex     = cfg.gpuIndex;
    m_containerFormat = cfg.containerFormat;
    m_audioMode    = cfg.audioMode;
    m_audioBitrate    = cfg.audioBitrate;
    m_outputFps       = cfg.outputFps;
    m_trueHdrEnabled    = cfg.trueHdrEnabled;
    m_thdrContrast      = cfg.thdrContrast;
    m_thdrSaturation    = cfg.thdrSaturation;
    m_thdrMiddleGray    = cfg.thdrMiddleGray;
    m_thdrMaxLuminance  = cfg.thdrMaxLuminance;
}

void MainWindow::SaveUIToConfig()
{
    VSRConfig& cfg = m_config.Get();

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
    cfg.audioMode    = m_audioMode;
    cfg.audioBitrate    = m_audioBitrate;
    cfg.outputFps       = m_outputFps;
    cfg.trueHdrEnabled  = m_trueHdrEnabled;
    cfg.thdrContrast    = m_thdrContrast;
    cfg.thdrSaturation  = m_thdrSaturation;
    cfg.thdrMiddleGray  = m_thdrMiddleGray;
    cfg.thdrMaxLuminance = m_thdrMaxLuminance;
}
