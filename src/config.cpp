#include "config.h"

Config::Config()  { Load(); }
Config::~Config() { Save(); }

std::wstring Config::GetIniPath() const {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'.');
    if (p) wcscpy(p, L".ini");
    return path;
}

void Config::Load() {
    auto path = GetIniPath();
    auto r = [&](const wchar_t* section, const wchar_t* key, int def) {
        return GetPrivateProfileIntW(section, key, def, path.c_str());
    };
    auto rs = [&](const wchar_t* section, const wchar_t* key, wchar_t* buf, const wchar_t* def) {
        GetPrivateProfileStringW(section, key, def, buf, MAX_PATH, path.c_str());
    };

    m_config.windowX   = r(L"Window", L"X", CW_USEDEFAULT);
    m_config.windowY   = r(L"Window", L"Y", CW_USEDEFAULT);
    m_config.windowW   = r(L"Window", L"W", 540);
    m_config.windowH   = r(L"Window", L"H", 506);
    m_config.qualityLevel = r(L"Params", L"Quality", 3);
    m_config.outputMode   = r(L"Params", L"OutputMode", 0);
    m_config.fixedWidth   = r(L"Params", L"FixedWidth", 3840);
    m_config.fixedHeight  = r(L"Params", L"FixedHeight", 2160);
    m_config.encoderIndex = r(L"Params", L"Encoder", 0);
    m_config.crf          = r(L"Params", L"CRF", 18);
    m_config.encoderSpeed = r(L"Params", L"Speed", 2);
    m_config.gpuIndex     = r(L"Params", L"GPU", 0);
    m_config.containerFormat = r(L"Params", L"Container", 0);
    m_config.outputFps    = r(L"Params", L"OutputFps", 0);
    m_config.audioMode = r(L"Params", L"AudioMode", 1);
    m_config.audioBitrate = r(L"Params", L"AudioBitrate", 128);

    rs(L"Paths", L"Input",  m_config.lastInputPath,  L"");
    rs(L"Paths", L"Output", m_config.lastOutputPath, L"");
}

void Config::Save() {
    auto path = GetIniPath();
    auto w = [&](const wchar_t* section, const wchar_t* key, int val) {
        wchar_t buf[16]; swprintf(buf, 16, L"%d", val);
        WritePrivateProfileStringW(section, key, buf, path.c_str());
    };
    w(L"Window", L"X", m_config.windowX);
    w(L"Window", L"Y", m_config.windowY);
    w(L"Window", L"W", m_config.windowW);
    w(L"Window", L"H", m_config.windowH);
    w(L"Params", L"Quality",   m_config.qualityLevel);
    w(L"Params", L"OutputMode", m_config.outputMode);
    w(L"Params", L"FixedWidth", m_config.fixedWidth);
    w(L"Params", L"FixedHeight", m_config.fixedHeight);
    w(L"Params", L"Encoder",   m_config.encoderIndex);
    w(L"Params", L"CRF",       m_config.crf);
    w(L"Params", L"Speed",     m_config.encoderSpeed);
    w(L"Params", L"GPU",       m_config.gpuIndex);
    w(L"Params", L"Container", m_config.containerFormat);
    w(L"Params", L"OutputFps", m_config.outputFps);
    w(L"Params", L"AudioMode", m_config.audioMode);
    w(L"Params", L"AudioBitrate", m_config.audioBitrate);

    WritePrivateProfileStringW(L"Paths", L"Input",  m_config.lastInputPath,  path.c_str());
    WritePrivateProfileStringW(L"Paths", L"Output", m_config.lastOutputPath, path.c_str());
}
