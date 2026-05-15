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

    char logPath[MAX_PATH];
    GetModuleFileNameA(NULL, logPath, sizeof(logPath));
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(logPath, "pipeline_debug.log");
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) { fprintf(f, "%s\n", msg); fflush(f); fclose(f); }

    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteConsoleA(hCon, msg, (DWORD)strlen(msg), &wrote, nullptr);
        WriteConsoleA(hCon, "\n", 1, &wrote, nullptr);
    }
}

static void LogStatus(const std::function<void(const char*)>& statusCb, const char* msg) {
    LogDbg(msg);
    if (statusCb) statusCb(msg);
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

static bool CudaFailed(const char* tag) {
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
            if (m_slots[i].nv12_cpu)     { cudaFreeHost(m_slots[i].nv12_cpu); m_slots[i].nv12_cpu = nullptr; }
            if (m_slots[i].nv12_out_cpu) { cudaFreeHost(m_slots[i].nv12_out_cpu); m_slots[i].nv12_out_cpu = nullptr; }
            if (m_slots[i].d_nv12)       cudaFree(m_slots[i].d_nv12);
            if (m_slots[i].d_rgba_src)   cudaFree(m_slots[i].d_rgba_src);
            if (m_slots[i].d_rgba_dst)   cudaFree(m_slots[i].d_rgba_dst);
            if (m_slots[i].d_nv12_out)   cudaFree(m_slots[i].d_nv12_out);
            if (m_slots[i].stream)       cudaStreamDestroy(m_slots[i].stream);
        }
        if (onError) onError(L"管道线程异常崩溃，请查看 pipeline_debug.log");
        m_state.store(PipelineState::Error);
        LogDbg("ThreadFunc SEH handler done");
    }
}

void PipelineController::DecodeFunc() {
    LogDbg("--- Decode thread started ---");

    cudaSetDevice(m_cfg.gpuIndex);
    LogDbg("Decode: CUDA device set");

    size_t nv12Size = (size_t)m_srcW * m_srcH * 3 / 2;
    int decFrameIdx = 0;

    while (true) {
        if (m_state.load() == PipelineState::Paused) {
            std::unique_lock<std::mutex> lk(m_pauseMutex);
            m_pauseCv.wait(lk, [] { return true; }); // will be re-checked after wait
        }
        while (m_state.load() == PipelineState::Paused) {
            Sleep(10);
        }
        if (m_state.load() != PipelineState::Running) {
            LogDbg("Decode: state not Running, exiting");
            break;
        }

        int slotIdx = -1;
        for (int i = 0; i < NUM_SLOTS; i++) {
            SlotState expected = SlotState::Empty;
            if (m_slots[i].state.compare_exchange_strong(expected, SlotState::Decoding)) {
                slotIdx = i;
                break;
            }
        }

        if (slotIdx < 0) {
            std::unique_lock<std::mutex> lk(m_slotMutex);
            m_slotCv.wait_for(lk, std::chrono::milliseconds(200));
            continue;
        }

        FrameSlot& slot = m_slots[slotIdx];

        int strides[2];
        if (!m_decoder.ReadFrameNV12(slot.nv12_cpu, strides)) {
            LogDbg("Decode: EOF or error");
            slot.state.store(SlotState::Empty);
            m_decodeDone.store(true);
            m_slotCv.notify_one();
            break;
        }

        cudaMemcpyAsync(slot.d_nv12, slot.nv12_cpu, nv12Size,
                        cudaMemcpyHostToDevice, slot.stream);
        launch_nv12_to_rgba(
            slot.d_nv12, m_srcW,
            slot.d_nv12 + m_srcW * m_srcH, m_srcW,
            slot.d_rgba_src, m_srcW * 4,
            m_srcW, m_srcH, slot.stream);
        cudaStreamSynchronize(slot.stream);
        if (CudaFailed("Decode: NV12->RGBA")) {
            LogDbg("Decode: CUDA error in NV12->RGBA");
            m_state.store(PipelineState::Error);
            slot.state.store(SlotState::Empty);
            m_slotCv.notify_one();
            break;
        }

        slot.state.store(SlotState::VSR_Ready);
        m_slotCv.notify_one();

        decFrameIdx++;
        if (decFrameIdx % 100 == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Decode: processed %d frames so far", decFrameIdx);
            LogDbg(buf);
        }
    }

    LogDbg("--- Decode thread finished ---");
}

static void SafeCleanup(VideoEncoder& enc, VSRProcessor& vsr, VideoDecoder& dec) {
    __try { enc.Close(); } __except (EXCEPTION_EXECUTE_HANDLER) { LogDbg("SEH in encoder.Close()"); }
    __try { vsr.Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) { LogDbg("SEH in vsr.Shutdown()"); }
    __try { dec.Close(); } __except (EXCEPTION_EXECUTE_HANDLER) { LogDbg("SEH in decoder.Close()"); }
}

void PipelineController::ThreadFuncImpl() {
    std::chrono::time_point<std::chrono::high_resolution_clock> lastReport;
    LogDbg("--- Pipeline GPU thread started ---");

    if (m_state.load() != PipelineState::Starting) {
        LogDbg("State not Starting, aborting");
        return;
    }

    // ---- CUDA runtime init ----
    LogStatus(onStatus, "CUDA 运行时初始化...");
    {
        cudaError_t initErr = cudaSetDevice(m_cfg.gpuIndex);
        if (initErr != cudaSuccess) {
            LogDbg("cudaSetDevice failed");
            if (onError) onError(L"CUDA 初始化失败");
            m_state.store(PipelineState::Error);
            return;
        }
        cudaFree(0);
        CudaFailed("cudaFree(0)");
        LogStatus(onStatus, "CUDA 运行时初始化完成");
    }

    // ---- Open decoder ----
    LogStatus(onStatus, "打开解码器...");
    VideoInfo info;
    if (!m_decoder.Open(m_cfg.inputPath.c_str(), &info)) {
        LogDbg("Failed to open input file");
        if (onError) onError(L"无法打开输入文件");
        m_state.store(PipelineState::Error);
        return;
    }
    LogStatus(onStatus, "解码器打开成功");

    m_srcW = info.width;
    m_srcH = info.height;
    m_srcFps = info.fps;
    m_totalFrames = info.totalFrames;
    if (m_totalFrames <= 0) m_totalFrames = 1;

    CalculateOutputSize(m_srcW, m_srcH, m_dstW, m_dstH);
    double outFps = m_cfg.outputFps > 0 ? (double)m_cfg.outputFps : m_srcFps;

    // ---- Allocate frame slots (pinned CPU memory + GPU + per-slot streams) ----
    LogStatus(onStatus, "分配帧缓冲区...");
    size_t nv12Size     = (size_t)m_srcW * m_srcH * 3 / 2;
    size_t nv12OutSize  = (size_t)m_dstW * m_dstH * 3 / 2;
    size_t rgbaSrcSize  = (size_t)m_srcW * m_srcH * 4;
    size_t rgbaDstSize  = (size_t)m_dstW * m_dstH * 4;

    { char buf[256]; snprintf(buf, sizeof(buf), "src=%dx%d dst=%dx%d nv12=%zu rgba=%zu",
        m_srcW, m_srcH, m_dstW, m_dstH, nv12Size, rgbaSrcSize); LogDbg(buf); }

    for (int i = 0; i < NUM_SLOTS; i++) {
        m_slots[i].w = m_srcW;
        m_slots[i].h = m_srcH;
        m_slots[i].dstW = m_dstW;
        m_slots[i].dstH = m_dstH;

        // Pinned (page-locked) CPU memory for high-speed DMA transfers
        if (cudaMallocHost(&m_slots[i].nv12_cpu, nv12Size) != cudaSuccess ||
            cudaMallocHost(&m_slots[i].nv12_out_cpu, nv12OutSize) != cudaSuccess) {
            LogDbg("Failed to allocate pinned CPU memory");
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

        // Non-blocking per-slot streams for GPU kernel overlap
        if (cudaStreamCreateWithFlags(&m_slots[i].stream, cudaStreamNonBlocking) != cudaSuccess) {
            LogDbg("Failed to create CUDA stream");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }

        m_slots[i].state.store(SlotState::Empty);
    }
    LogStatus(onStatus, "帧缓冲区分配完成");

    // ---- Audio ----
    m_audioPackets.clear();
    m_decoder.SetAudioPacketQueue(&m_audioPackets);

    // ---- Open VSR (SEH-safe) ----
    LogStatus(onStatus, "初始化 VSR (NGX)...");
    if (!SafeVSRInit(&m_vsr, m_cfg.gpuIndex)) {
        LogDbg("VSR init failed");
        if (onError) onError(L"VSR 初始化失败，请检查 GPU 和驱动（需要 550+）");
        m_state.store(PipelineState::Error);
        goto cleanup;
    }
    LogStatus(onStatus, "VSR 初始化完成");

    // ---- Open encoder ----
    LogStatus(onStatus, "打开编码器...");
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
        encCfg.audioMode  = m_cfg.audioMode;
        encCfg.audioBitrate  = m_cfg.audioBitrate;
        encCfg.audioStreamIdx = info.audioStreamIndex;
        encCfg.audioSampleRate = info.audioSampleRate;
        encCfg.audioChannels = info.audioChannels;
        encCfg.audioPackets  = &m_audioPackets;
        encCfg.audioCodecPar = m_decoder.GetAudioCodecPar();

        if (!m_encoder.Open(encCfg, onStatus)) {
            LogDbg("Failed to open encoder");
            if (onError) onError(L"无法打开编码器");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }
    }
    LogStatus(onStatus, "编码器打开成功");

    // ---- Launch decode thread ----
    m_decodeDone.store(false);
    m_framesEncoded.store(0);
    LogDbg("Step: Launching decode thread...");
    m_decodeThread = std::thread(&PipelineController::DecodeFunc, this);
    LogDbg("Decode thread launched");

    // ---- GPU thread main loop (VSR + encode) ----
    m_state.store(PipelineState::Running);
    LogStatus(onStatus, "开始 GPU 处理循环");

    VSRFrameContext vsrCtx;
    vsrCtx.vsr     = &m_vsr;
    vsrCtx.quality = (VSRQuality)m_cfg.qualityLevel;

    lastReport = std::chrono::high_resolution_clock::now();

    while (true) {
        if (m_state.load() == PipelineState::Paused) {
            std::unique_lock<std::mutex> lk(m_pauseMutex);
            m_pauseCv.wait(lk, [] { return true; });
        }
        while (m_state.load() == PipelineState::Paused) {
            Sleep(10);
        }
        if (m_state.load() != PipelineState::Running) {
            LogDbg("GPU: state not Running, exiting loop");
            break;
        }

        // Search for VSR_Ready slot
        int slotIdx = -1;
        {
            std::unique_lock<std::mutex> lk(m_slotMutex);
            m_slotCv.wait_for(lk, std::chrono::milliseconds(100), [&] {
                for (int i = 0; i < NUM_SLOTS; i++) {
                    if (m_slots[i].state.load() == SlotState::VSR_Ready) return true;
                }
                return m_decodeDone.load();
            });
        }

        for (int i = 0; i < NUM_SLOTS; i++) {
            SlotState expected = SlotState::VSR_Ready;
            if (m_slots[i].state.compare_exchange_strong(expected, SlotState::Encoding)) {
                slotIdx = i;
                break;
            }
        }

        if (slotIdx < 0) {
            if (m_decodeDone.load()) {
                bool allDone = true;
                for (int i = 0; i < NUM_SLOTS; i++) {
                    SlotState s = m_slots[i].state.load();
                    if (s != SlotState::Empty) allDone = false;
                }
                if (allDone) {
                    LogDbg("GPU: all slots done, exiting loop");
                    break;
                }
            }
            continue;
        }

        FrameSlot& slot = m_slots[slotIdx];

        // VSR process (SEH-safe)
        vsrCtx.src  = slot.d_rgba_src;
        vsrCtx.dst  = slot.d_rgba_dst;
        vsrCtx.srcW = m_srcW;
        vsrCtx.srcH = m_srcH;
        vsrCtx.dstW = m_dstW;
        vsrCtx.dstH = m_dstH;

        if (!SafeVSRProcess(&vsrCtx)) {
            LogDbg("VSR evaluate failed or crashed");
            if (onError) onError(L"VSR 处理失败");
            m_state.store(PipelineState::Error);
            break;
        }

        // RGBA→NV12 kernel + D2H copy on per-slot stream
        launch_rgba_to_nv12(
            slot.d_rgba_dst, m_dstW * 4,
            slot.d_nv12_out, m_dstW,
            slot.d_nv12_out + m_dstW * m_dstH, m_dstW,
            m_dstW, m_dstH, slot.stream);
        cudaMemcpyAsync(slot.nv12_out_cpu, slot.d_nv12_out, nv12OutSize,
                        cudaMemcpyDeviceToHost, slot.stream);
        cudaStreamSynchronize(slot.stream);
        if (CudaFailed("GPU: RGBA->NV12 + D2H")) {
            LogDbg("GPU: CUDA error in RGBA->NV12 or D2H");
            m_state.store(PipelineState::Error);
            break;
        }

        // Encode
        if (!m_encoder.WriteFrameNV12(slot.nv12_out_cpu, m_dstW, m_dstW)) {
            LogDbg("Encode failed");
            if (onError) onError(L"编码失败");
            m_state.store(PipelineState::Error);
            break;
        }

        // Mark slot as empty, notify decoder
        slot.state.store(SlotState::Empty);
        m_slotCv.notify_one();

        int encoded = m_framesEncoded.fetch_add(1) + 1;

        // Progress reporting
        auto now = std::chrono::high_resolution_clock::now();
        float ms = std::chrono::duration<float, std::milli>(now - lastReport).count();
        if (ms > 0 && onProgress) {
            PipelineProgress p;
            p.currentFrame   = encoded;
            p.totalFrames    = m_totalFrames;
            p.fps            = 1000.0f / ms;
            p.avgMsPerFrame  = ms;
            p.etaSeconds     = (m_totalFrames - encoded) * ms / 1000.0f;
            onProgress(p);
        }
        lastReport = now;

        if (encoded % 100 == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Encoded %d frames so far", encoded);
            LogDbg(buf);
        }
    }

    { char buf[128]; snprintf(buf, sizeof(buf), "GPU loop ended, total frames encoded: %d",
        m_framesEncoded.load()); LogDbg(buf); }

    // ---- Wait for decode thread ----
    LogDbg("Waiting for decode thread to join...");
    if (m_decodeThread.joinable()) {
        m_decodeDone.store(true);
        m_slotCv.notify_all();
        m_decodeThread.join();
    }
    LogDbg("Decode thread joined");

cleanup:
    LogStatus(onStatus, "清理资源...");
    SafeCleanup(m_encoder, m_vsr, m_decoder);

    for (auto* p : m_audioPackets) {
        if (p) { AVPacket* ap = static_cast<AVPacket*>(p); av_packet_free(&ap); }
    }
    m_audioPackets.clear();

    LogStatus(onStatus, "释放 GPU 内存和流...");
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (m_slots[i].nv12_cpu)     { cudaFreeHost(m_slots[i].nv12_cpu); m_slots[i].nv12_cpu = nullptr; }
        if (m_slots[i].nv12_out_cpu) { cudaFreeHost(m_slots[i].nv12_out_cpu); m_slots[i].nv12_out_cpu = nullptr; }
        if (m_slots[i].d_nv12)       cudaFree(m_slots[i].d_nv12);
        if (m_slots[i].d_rgba_src)   cudaFree(m_slots[i].d_rgba_src);
        if (m_slots[i].d_rgba_dst)   cudaFree(m_slots[i].d_rgba_dst);
        if (m_slots[i].d_nv12_out)   cudaFree(m_slots[i].d_nv12_out);
        m_slots[i].d_rgba_src = m_slots[i].d_rgba_dst = nullptr;
        m_slots[i].d_nv12 = m_slots[i].d_nv12_out = nullptr;
        if (m_slots[i].stream) { cudaStreamDestroy(m_slots[i].stream); m_slots[i].stream = nullptr; }
    }

    if (m_state.load() == PipelineState::Running) {
        m_state.store(PipelineState::Completed);
        if (onCompleted) onCompleted();
    } else if (m_state.load() == PipelineState::Starting) {
        m_state.store(PipelineState::Idle);
    } else if (m_state.load() == PipelineState::Paused) {
        m_state.store(PipelineState::Idle);
    }
    LogDbg("Pipeline GPU thread finished");
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
    LogStatus(onStatus, "管道线程已启动");
    return true;
}

void PipelineController::Pause() {
    PipelineState expected = PipelineState::Running;
    if (m_state.compare_exchange_strong(expected, PipelineState::Paused)) {
        m_pauseCv.notify_all();
    }
}

void PipelineController::Resume() {
    PipelineState expected = PipelineState::Paused;
    if (m_state.compare_exchange_strong(expected, PipelineState::Running)) {
        m_pauseCv.notify_all();
    }
}

void PipelineController::Stop() {
    m_state.store(PipelineState::Idle);
    m_pauseCv.notify_all();
    m_slotCv.notify_all();
    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    LogStatus(onStatus, "管道已停止");
}