#include "config.h"

Config::Config()  { Load(); }
Config::~Config() { Save(); }

std::wstring Config::GetIniPath() const {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'.');
    if (p) {
        wcscpy(p, L".ini");
    } else {
        size_t len = wcslen(path);
        if (len + 5 <= MAX_PATH)
            wcscat_s(path, L".ini");
    }
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

    m_window.windowW = r(L"Window", L"W", 540);
    m_window.windowH = r(L"Window", L"H", 506);

    m_pipelineCfg.qualityLevel     = r(L"Params", L"Quality", 3);
    m_pipelineCfg.outputMode       = r(L"Params", L"OutputMode", 0);
    m_pipelineCfg.outputWidth      = r(L"Params", L"FixedWidth", 3840);
    m_pipelineCfg.outputHeight     = r(L"Params", L"FixedHeight", 2160);
    m_pipelineCfg.encoderIndex     = r(L"Params", L"Encoder", 0);
    m_pipelineCfg.crf              = r(L"Params", L"CRF", 18);
    m_pipelineCfg.encoderSpeed     = r(L"Params", L"Speed", 3);
    m_pipelineCfg.gpuIndex         = r(L"Params", L"GPU", 0);
    m_pipelineCfg.container        = r(L"Params", L"Container", 0);
    m_pipelineCfg.audioMode        = r(L"Params", L"AudioMode", 1);
    m_pipelineCfg.audioBitrate     = r(L"Params", L"AudioBitrate", 128);
    m_pipelineCfg.trueHdrEnabled   = r(L"Params", L"TrueHdr", 0) != 0;
    m_pipelineCfg.thdrContrast     = r(L"Params", L"THDR_Contrast", 100);
    m_pipelineCfg.thdrSaturation   = r(L"Params", L"THDR_Saturation", 100);
    m_pipelineCfg.thdrMiddleGray   = r(L"Params", L"THDR_MiddleGray", 50);
    m_pipelineCfg.thdrMaxLuminance = r(L"Params", L"THDR_MaxLuminance", 1000);

    rs(L"Paths", L"Input",  m_window.lastInputPath,  L"");
    rs(L"Paths", L"Output", m_window.lastOutputPath, L"");
}

void Config::Save() {
    auto path = GetIniPath();
    auto w = [&](const wchar_t* section, const wchar_t* key, int val) {
        wchar_t buf[16]; swprintf(buf, 16, L"%d", val);
        WritePrivateProfileStringW(section, key, buf, path.c_str());
    };
    w(L"Window", L"W", m_window.windowW);
    w(L"Window", L"H", m_window.windowH);

    w(L"Params", L"Quality",        m_pipelineCfg.qualityLevel);
    w(L"Params", L"OutputMode",     m_pipelineCfg.outputMode);
    w(L"Params", L"FixedWidth",     m_pipelineCfg.outputWidth);
    w(L"Params", L"FixedHeight",    m_pipelineCfg.outputHeight);
    w(L"Params", L"Encoder",        m_pipelineCfg.encoderIndex);
    w(L"Params", L"CRF",            m_pipelineCfg.crf);
    w(L"Params", L"Speed",          m_pipelineCfg.encoderSpeed);
    w(L"Params", L"GPU",            m_pipelineCfg.gpuIndex);
    w(L"Params", L"Container",      m_pipelineCfg.container);
    w(L"Params", L"AudioMode",      m_pipelineCfg.audioMode);
    w(L"Params", L"AudioBitrate",   m_pipelineCfg.audioBitrate);
    w(L"Params", L"TrueHdr",        m_pipelineCfg.trueHdrEnabled ? 1 : 0);
    w(L"Params", L"THDR_Contrast",    m_pipelineCfg.thdrContrast);
    w(L"Params", L"THDR_Saturation",  m_pipelineCfg.thdrSaturation);
    w(L"Params", L"THDR_MiddleGray",  m_pipelineCfg.thdrMiddleGray);
    w(L"Params", L"THDR_MaxLuminance", m_pipelineCfg.thdrMaxLuminance);

    WritePrivateProfileStringW(L"Paths", L"Input",  m_window.lastInputPath,  path.c_str());
    WritePrivateProfileStringW(L"Paths", L"Output", m_window.lastOutputPath, path.c_str());
}
