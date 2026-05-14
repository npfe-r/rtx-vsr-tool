#include "pipeline_ctrl.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <windows.h>
#include <exception>

extern "C" {
#include <libavcodec/avcodec.h>
}

static void LogDbg(const char* msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#ifdef _DEBUG
    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "%s\n", msg); fflush(f); fclose(f); }
#endif
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}

static LONG WINAPI PipelineUnhandledFilter(_EXCEPTION_POINTERS* ep) {
    LogDbg("!!! UNHANDLED EXCEPTION in pipeline thread !!!");
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    char buf[128];
    snprintf(buf, sizeof(buf), "ExceptionCode=0x%08lX addr=%p",
             code, ep->ExceptionRecord->ExceptionAddress);
    LogDbg(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

// SEH-safe wrappers for NGX calls (no C++ objects in __try function)
struct VSRFrameContext {
    VSRProcessor* vsr;
    const void* src;
    void* dst;
    int srcW, srcH, dstW, dstH;
    VSRQuality quality;
};

static bool SafeVSRInit(VSRProcessor* vsr, int gpuIndex)
{
    LogDbg("  -> calling vsr->Initialize()...");
    __try {
        bool ok = vsr->Initialize(gpuIndex);
        LogDbg("  -> vsr->Initialize() returned");
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogDbg("!!! SEH CRASH in VSR Initialize !!!");
        return false;
    }
}

static bool SafeVSRProcess(VSRFrameContext* ctx)
{
    __try {
        return ctx->vsr->ProcessFrame(ctx->src, ctx->dst,
                                       ctx->srcW, ctx->srcH,
                                       ctx->dstW, ctx->dstH,
                                       ctx->quality);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogDbg("!!! SEH CRASH in VSR ProcessFrame !!!");
        return false;
    }
}

PipelineController::PipelineController() {}
PipelineController::~PipelineController() { Stop(); }

void PipelineController::CalculateOutputSize(int srcW, int srcH, int& dstW, int& dstH) const {
    switch (m_cfg.outputMode) {
        case 0: dstW = srcW * 2; dstH = srcH * 2; break;
        case 1: dstW = srcW * 4; dstH = srcH * 4; break;
        case 2: dstW = m_cfg.outputWidth;  dstH = m_cfg.outputHeight; break;
        default: dstW = srcW * 2; dstH = srcH * 2; break;
    }
    dstW = (dstW + 15) & ~15;
    dstH = (dstH + 15) & ~15;
}

static bool CheckCuda(const char* tag) {
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        char buf[256];
        snprintf(buf, sizeof(buf), "CUDA error at %s: %s", tag, cudaGetErrorString(err));
        LogDbg(buf);
        return true;
    }
    return false;
}

void PipelineController::ThreadFunc() {
    SetUnhandledExceptionFilter(PipelineUnhandledFilter);

    __try {
        ThreadFuncImpl();
        LogDbg("ThreadFunc: ThreadFuncImpl returned normally");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogDbg("SEH EXCEPTION CAUGHT in pipeline thread!");
        for (int i = 0; i < NUM_SLOTS; i++) {
            free(m_slots[i].nv12_cpu);
            free(m_slots[i].nv12_out_cpu);
            m_slots[i].nv12_cpu = nullptr;
            m_slots[i].nv12_out_cpu = nullptr;
        }
        if (onError) onError(L"管道线程异常崩溃，请查看 pipeline_debug.log");
        m_state.store(PipelineState::Error);
        LogDbg("ThreadFunc SEH handler done");
    }
}

void PipelineController::ThreadFuncImpl() {

        LogDbg("--- Pipeline thread started ---");

        if (m_state.load() != PipelineState::Starting) {
            LogDbg("State not Starting, aborting");
            return;
        }

        // ---- Explicitly init CUDA runtime ----
        LogDbg("Step: CUDA runtime init...");
        {
            cudaError_t initErr = cudaSetDevice(m_cfg.gpuIndex);
            if (initErr != cudaSuccess) {
                LogDbg("cudaSetDevice failed");
                if (onError) onError(L"CUDA 初始化失败");
                m_state.store(PipelineState::Error);
                return;
            }
            cudaFree(0);
            CheckCuda("cudaFree(0)");
            LogDbg("CUDA runtime initialized");
        }

        // ---- Open decoder ----
        LogDbg("Step: Open decoder...");
        VideoInfo info;
        if (!m_decoder.Open(m_cfg.inputPath.c_str(), &info)) {
            LogDbg("Failed to open input file");
            if (onError) onError(L"无法打开输入文件");
            m_state.store(PipelineState::Error);
            return;
        }
        LogDbg("Decoder opened successfully");

        m_srcW = info.width;
        m_srcH = info.height;
        m_srcFps = info.fps;
        m_totalFrames = info.totalFrames;
        if (m_totalFrames <= 0) m_totalFrames = 1;

        CalculateOutputSize(m_srcW, m_srcH, m_dstW, m_dstH);
        double outFps = m_cfg.outputFps > 0 ? (double)m_cfg.outputFps : m_srcFps;

        // ---- Allocate frame slots ----
        LogDbg("Step: Allocate frame slots...");
        size_t nv12Size     = (size_t)m_srcW * m_srcH * 3 / 2;
        size_t nv12OutSize  = (size_t)m_dstW * m_dstH * 3 / 2;
        size_t rgbaSrcSize  = (size_t)m_srcW * m_srcH * 4;
        size_t rgbaDstSize  = (size_t)m_dstW * m_dstH * 4;
        int frameIdx = 0;
        auto lastReport = std::chrono::high_resolution_clock::now();

        { char buf[256]; snprintf(buf, sizeof(buf), "src=%dx%d dst=%dx%d nv12=%zu rgba=%zu", m_srcW, m_srcH, m_dstW, m_dstH, nv12Size, rgbaSrcSize); LogDbg(buf); }

        for (int i = 0; i < NUM_SLOTS; i++) {
            m_slots[i].w = m_srcW;
            m_slots[i].h = m_srcH;
            m_slots[i].dstW = m_dstW;
            m_slots[i].dstH = m_dstH;
            m_slots[i].nv12_cpu     = (uint8_t*)malloc(nv12Size);
            m_slots[i].nv12_out_cpu = (uint8_t*)malloc(nv12OutSize);
            if (!m_slots[i].nv12_cpu || !m_slots[i].nv12_out_cpu) {
                LogDbg("Failed to allocate CPU memory");
                if (onError) onError(L"内存分配失败");
                m_state.store(PipelineState::Error);
                goto cleanup;
            }
            if (cudaMalloc(&m_slots[i].d_nv12, nv12Size) != cudaSuccess ||
                cudaMalloc(&m_slots[i].d_rgba_src, rgbaSrcSize) != cudaSuccess ||
                cudaMalloc(&m_slots[i].d_rgba_dst, rgbaDstSize) != cudaSuccess ||
                cudaMalloc(&m_slots[i].d_nv12_out, nv12OutSize) != cudaSuccess) {
                LogDbg("Failed to allocate GPU memory");
                if (onError) onError(L"GPU 内存分配失败，请检查显存");
                m_state.store(PipelineState::Error);
                goto cleanup;
            }
        }
        LogDbg("Frame slots allocated");

        // ---- Audio ----
        m_audioPackets.clear();
        m_decoder.SetAudioPacketQueue(&m_audioPackets);

        // ---- Open VSR (SEH-safe via helper) ----
        LogDbg("Step: Initialize VSR (NGX)...");
        if (!SafeVSRInit(&m_vsr, m_cfg.gpuIndex)) {
            LogDbg("VSR init failed");
            if (onError) onError(L"VSR 初始化失败，请检查 GPU 和驱动（需要 550+）");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }
        LogDbg("VSR initialized");

        // ---- Open encoder ----
        LogDbg("Step: Open encoder...");
        {
            EncodeConfig encCfg;
            wchar_t outputPath[MAX_PATH];
            wcsncpy(outputPath, m_cfg.outputPath.c_str(), MAX_PATH);
            encCfg.outputPath    = outputPath;
            encCfg.width         = m_dstW;
            encCfg.height        = m_dstH;
            encCfg.fps           = outFps;
            encCfg.codecId       = m_cfg.encoderIndex;
            encCfg.crf           = m_cfg.crf;
            encCfg.speed         = m_cfg.encoderSpeed;
            encCfg.container     = m_cfg.container;
            encCfg.hasAudio      = info.hasAudio;
            encCfg.audioStreamIdx = info.audioStreamIndex;
            encCfg.audioSampleRate = info.audioSampleRate;
            encCfg.audioChannels = info.audioChannels;
            encCfg.audioPackets  = &m_audioPackets;
            encCfg.audioCodecPar = m_decoder.GetAudioCodecPar();

            if (!m_encoder.Open(encCfg)) {
                LogDbg("Failed to open encoder");
                if (onError) onError(L"无法打开编码器");
                m_state.store(PipelineState::Error);
                goto cleanup;
            }
        }
        LogDbg("Encoder opened");

        // ---- Main loop ----
        m_state.store(PipelineState::Running);
        LogDbg("Step: Entering main processing loop");

        VSRFrameContext vsrCtx;
        vsrCtx.vsr     = &m_vsr;
        vsrCtx.quality = (VSRQuality)m_cfg.qualityLevel;

        while (m_state.load() == PipelineState::Running ||
               m_state.load() == PipelineState::Paused) {

            if (m_state.load() == PipelineState::Paused) {
                std::unique_lock<std::mutex> lk(m_cvMutex);
                m_cv.wait(lk, [this] {
                    return m_state.load() != PipelineState::Paused;
                });
            }

            if (m_state.load() != PipelineState::Running) break;

            int slotIdx = frameIdx % NUM_SLOTS;
            FrameSlot& slot = m_slots[slotIdx];

            int strides[2];
            if (!m_decoder.ReadFrameNV12(slot.nv12_cpu, strides)) {
                LogDbg("Decoder EOF or error");
                break;
            }

            cudaStream_t stream = nullptr;

            cudaMemcpyAsync(slot.d_nv12, slot.nv12_cpu, nv12Size,
                            cudaMemcpyHostToDevice, stream);
            if (CheckCuda("cudaMemcpyAsync H2D")) break;

            launch_nv12_to_rgba(
                slot.d_nv12, m_srcW,
                slot.d_nv12 + m_srcW * m_srcH, m_srcW,
                slot.d_rgba_src, m_srcW * 4,
                m_srcW, m_srcH, stream);
            if (CheckCuda("launch_nv12_to_rgba")) break;

            cudaStreamSynchronize(stream);
            if (CheckCuda("cudaStreamSynchronize")) break;

            // VSR process (SEH-safe via helper, no C++ dtors in __try)
            vsrCtx.src  = slot.d_rgba_src;
            vsrCtx.dst  = slot.d_rgba_dst;
            vsrCtx.srcW = m_srcW;
            vsrCtx.srcH = m_srcH;
            vsrCtx.dstW = m_dstW;
            vsrCtx.dstH = m_dstH;

            if (frameIdx == 0) LogDbg("First frame: calling VSR ProcessFrame...");

            if (!SafeVSRProcess(&vsrCtx)) {
                LogDbg("VSR evaluate failed or crashed");
                if (onError) onError(L"VSR 处理失败");
                m_state.store(PipelineState::Error);
                break;
            }

            if (frameIdx == 0) LogDbg("First VSR frame done");

            launch_rgba_to_nv12(
                slot.d_rgba_dst, m_dstW * 4,
                slot.d_nv12_out, m_dstW,
                slot.d_nv12_out + m_dstW * m_dstH, m_dstW,
                m_dstW, m_dstH, stream);
            if (CheckCuda("launch_rgba_to_nv12")) break;

            cudaMemcpyAsync(slot.nv12_out_cpu, slot.d_nv12_out, nv12OutSize,
                            cudaMemcpyDeviceToHost, stream);
            if (CheckCuda("cudaMemcpyAsync D2H")) break;

            cudaStreamSynchronize(stream);
            if (CheckCuda("cudaStreamSynchronize D2H")) break;

            if (!m_encoder.WriteFrameNV12(slot.nv12_out_cpu, m_dstW, m_dstW)) {
                LogDbg("Encode failed");
                if (onError) onError(L"编码失败");
                m_state.store(PipelineState::Error);
                break;
            }

            frameIdx++;

            if (frameIdx % 100 == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Processed %d frames so far", frameIdx);
                LogDbg(buf);
            }

            auto now = std::chrono::high_resolution_clock::now();
            float ms = std::chrono::duration<float, std::milli>(now - lastReport).count();
            if (ms > 0 && onProgress) {
                PipelineProgress p;
                p.currentFrame   = frameIdx;
                p.totalFrames    = m_totalFrames;
                p.fps            = 1000.0f / ms;
                p.avgMsPerFrame  = ms;
                p.etaSeconds     = (m_totalFrames - frameIdx) * ms / 1000.0f;
                onProgress(p);
            }
            lastReport = now;
        }

        { char buf[128]; snprintf(buf, sizeof(buf), "Main loop ended, total frames processed: %d", frameIdx); LogDbg(buf); }

    cleanup:
        LogDbg("Cleanup: freeing resources");
        m_encoder.Close();
        m_vsr.Shutdown();
        m_decoder.Close();

        for (auto* p : m_audioPackets) {
            if (p) { AVPacket* ap = static_cast<AVPacket*>(p); av_packet_free(&ap); }
        }
        m_audioPackets.clear();

        LogDbg("Cleanup: freeing GPU memory");
        for (int i = 0; i < NUM_SLOTS; i++) {
            free(m_slots[i].nv12_cpu);
            free(m_slots[i].nv12_out_cpu);
            if (m_slots[i].d_nv12) cudaFree(m_slots[i].d_nv12);
            if (m_slots[i].d_rgba_src) cudaFree(m_slots[i].d_rgba_src);
            if (m_slots[i].d_rgba_dst) cudaFree(m_slots[i].d_rgba_dst);
            if (m_slots[i].d_nv12_out) cudaFree(m_slots[i].d_nv12_out);
            m_slots[i].d_rgba_src = m_slots[i].d_rgba_dst = nullptr;
            m_slots[i].d_nv12 = m_slots[i].d_nv12_out = nullptr;
            m_slots[i].nv12_cpu = m_slots[i].nv12_out_cpu = nullptr;
        }

        if (m_state.load() == PipelineState::Running) {
            m_state.store(PipelineState::Completed);
            if (onCompleted) onCompleted();
        } else if (m_state.load() == PipelineState::Starting) {
            m_state.store(PipelineState::Idle);
        } else if (m_state.load() == PipelineState::Paused) {
            m_state.store(PipelineState::Idle);
        }
        LogDbg("Pipeline thread finished");
}

bool PipelineController::Start(const PipelineConfig& cfg) {
    PipelineState expected = PipelineState::Idle;
    if (!m_state.compare_exchange_strong(expected, PipelineState::Starting))
        return false;

    m_cfg = cfg;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_thread = std::thread(&PipelineController::ThreadFunc, this);
    LogDbg("Pipeline thread started");
    return true;
}

void PipelineController::Pause() {
    PipelineState expected = PipelineState::Running;
    m_state.compare_exchange_strong(expected, PipelineState::Paused);
}

void PipelineController::Resume() {
    PipelineState expected = PipelineState::Paused;
    if (m_state.compare_exchange_strong(expected, PipelineState::Running)) {
        m_cv.notify_all();
    }
}

void PipelineController::Stop() {
    m_state.store(PipelineState::Idle);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    LogDbg("Pipeline stopped");
}
