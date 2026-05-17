#pragma once
#include <windows.h>
#include <cstdio>

inline void LogMsg(const char* tag, const char* msg) {
    OutputDebugStringA(tag);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    wchar_t widePath[4096];
    DWORD len = GetModuleFileNameW(NULL, widePath, 4096);
    if (len > 0 && len < 4096) {
        wchar_t* slash = wcsrchr(widePath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscat_s(widePath, L"pipeline_debug.log");
        char logPath[4096];
        WideCharToMultiByte(CP_UTF8, 0, widePath, -1, logPath, 4096, nullptr, nullptr);
        FILE* f = nullptr;
        fopen_s(&f, logPath, "a");
        if (f) { fprintf(f, "%s%s\n", tag, msg); fflush(f); fclose(f); }
    }

    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, tag, (DWORD)strlen(tag), &wrote, nullptr);
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}
