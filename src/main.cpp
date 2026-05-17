#include <windows.h>
#include <cstdio>
#include "main_window.h"
#include "debug_util.h"

// __try/__except must be in a function with no C++ objects needing unwinding
static void RunMessageLoop(MainWindow& window)
{
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            __try {
                DispatchMessageW(&msg);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LogMsg("UI: ","!!! CRASH in DispatchMessageW !!!");
            }
        } else {
            __try {
                window.Render();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LogMsg("UI: ","!!! CRASH in Render() !!!");
            }
        }
    }
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // Declare DPI awareness before any window creation
    SetProcessDPIAware();

    // Clear previous log so each run starts fresh
    {
        wchar_t widePath[4096];
        DWORD len = GetModuleFileNameW(NULL, widePath, 4096);
        if (len > 0 && len < 4096) {
            wchar_t* slash = wcsrchr(widePath, L'\\');
            if (slash) *(slash + 1) = L'\0';
            wcscat_s(widePath, L"pipeline_debug.log");
            char logPath[4096];
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, logPath, 4096, nullptr, nullptr);
            FILE* f = nullptr;
            fopen_s(&f, logPath, "w");
            if (f) fclose(f);
        }
    }

#ifdef _DEBUG
    AllocConsole();
    LogMsg("UI: ","RTX VSR Tool starting...");
#endif

    MainWindow window;
    if (!window.Create(hInstance, nCmdShow)) {
        LogMsg("UI: ","ERROR: MainWindow::Create() failed");
        MessageBoxW(nullptr,
                    L"窗口或 DirectX 11 初始化失败\n\n"
                    L"请确认:\n"
                    L"• 显卡支持 DirectX 11\n"
                    L"• 已安装最新显卡驱动\n"
                    L"• 未使用远程桌面连接",
                    L"RTX VSR 错误", MB_ICONERROR);
        LogMsg("UI: ","Exiting...");
#ifdef _DEBUG
        system("pause");
#endif
        return 1;
    }

    LogMsg("UI: ","MainWindow created, entering message loop");

    // -autostart: kick off pipeline immediately (headless-friendly)
    {
        int ac = 0;
        LPWSTR* aw = CommandLineToArgvW(GetCommandLineW(), &ac);
        for (int i = 1; i < ac; i++) {
            if (wcscmp(aw[i], L"-autostart") == 0) {
                LogMsg("UI: ","autostart triggered");
                window.AutoStart();
                break;
            }
        }
        LocalFree(aw);
    }

    RunMessageLoop(window);

    LogMsg("UI: ","RTX VSR Tool exiting normally");
    return 0;
}
