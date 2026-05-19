#pragma once
#include <windows.h>
#include <string>
#include "pipeline_ctrl.h"

// Persisted window position and last-used paths (separate from PipelineConfig
// since these are GUI-only state that the pipeline doesn't consume).
struct WindowState {
    int windowW = 540;
    int windowH = 506;
    wchar_t lastInputPath[MAX_PATH] = L"";
    wchar_t lastOutputPath[MAX_PATH] = L"";
};

class Config {
public:
    Config();
    ~Config();

    void Load();
    void Save();

    PipelineConfig& GetPipeline() { return m_pipelineCfg; }
    WindowState&    GetWindow()   { return m_window; }

private:
    std::wstring GetIniPath() const;
    WindowState     m_window;
    PipelineConfig  m_pipelineCfg;
};
