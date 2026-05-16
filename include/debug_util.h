#pragma once
#include <windows.h>
#include <cstdio>

inline void LogMsg(const char* tag, const char* msg) {
    OutputDebugStringA(tag);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "%s%s\n", tag, msg); fflush(f); fclose(f); }

    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, tag, (DWORD)strlen(tag), &wrote, nullptr);
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}
