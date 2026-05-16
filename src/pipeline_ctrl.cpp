#include "pipeline_ctrl.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <climits>
#include <windows.h>
#include <exception>

#include "debug_util.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

static void LogStatus(const std::function<void(const char*)>& statusCb, const char* msg) {
    LogMsg("PIP: ",msg);
    if (statusCb) statusCb(msg);
}

static LONG WINAPI PipelineUnhandledFilter(_EXCEPTION_POINTERS* ep) {
    LogMsg("PIP: ","!!! UNHANDLED EXCEPTION in pipeline thread !!!");
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    char buf[128];
    snprintf(buf, sizeof(buf), "ExceptionCode=0x%08lX addr=%p",
             code, ep->ExceptionRecord->ExceptionAddress);
    LogMsg("PIP: ",buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

struct VSRFrameContext {
    VSRProcessor* vsr;
    const void* src;
    void* dst;
    int srcW, srcH, dstW, dstH;
    VSRQuality quality;
};

static bool SafeVSRInit(VSRProcessor* vsr, int gpuIndex, bool enableTrueHdr)
{
    LogMsg("PIP: ","  -> calling vsr->Initialize()...");
    __try {
        bool ok = vsr->Initialize(gpuIndex, enableTrueHdr);
        LogMsg("PIP: ","  -> vsr->Initialize() returned");
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMsg("PIP: ","!!! SEH CRASH in VSR Initialize !!!");
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
        LogMsg("PIP: ","!!! SEH CRASH in VSR ProcessFrame !!!");
        return false;
    }
}

PipelineController::PipelineController() {}
PipelineController::~PipelineController() {
    Stop();
    VSRProcessor::GlobalShutdown();
}

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
        LogMsg("PIP: ",buf);
        return true;
    }
    return false;
}

void PipelineController::ThreadFunc() {
    SetUnhandledExceptionFilter(PipelineUnhandledFilter);

    __try {
        ThreadFuncImpl();
        LogMsg("PIP: ","ThreadFunc: ThreadFuncImpl returned normally");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMsg("PIP: ","SEH EXCEPTION CAUGHT in pipeline thread!");
        for (int i = 0; i < NUM_SLOTS; i++) {
            if (m_slots[i].nv12_cpu)     { cudaFreeHost(m_slots[i].nv12_cpu); m_slots[i].nv12_cpu = nullptr; }
            if (m_slots[i].nv12_out_cpu) { cudaFreeHost(m_slots[i].nv12_out_cpu); m_slots[i].nv12_out_cpu = nullptr; }
            if (m_slots[i].d_nv12)       cudaFree(m_slots[i].d_nv12);
            if (m_slots[i].d_rgba_src)   cudaFree(m_slots[i].d_rgba_src);
            if (m_slots[i].d_rgba_dst)   cudaFree(m_slots[i].d_rgba_dst);
            if (m_slots[i].d_nv12_out)   cudaFree(m_slots[i].d_nv12_out);
            if (m_slots[i].decodeEvent)  cudaEventDestroy(m_slots[i].decodeEvent);
            if (m_slots[i].stream)       cudaStreamDestroy(m_slots[i].stream);
        }
        if (onError) onError(L"管道线程异常崩溃，请查看 pipeline_debug.log");
        m_state.store(PipelineState::Error);
        LogMsg("PIP: ","ThreadFunc SEH handler done");
    }
}

void PipelineController::DecodeFunc() {
    LogMsg("PIP: ","--- Decode thread started ---");

    cudaSetDevice(m_cfg.gpuIndex);
    LogMsg("PIP: ","Decode: CUDA device set");

    // Decode path: NVDEC GPU (device pointers directly) or CPU software (NV12→H2D).
    // GPU path uses an internal PTS reorder buffer (depth=8) and
    // returns frames in display order regardless of the decode-order arrival.
    int decFrameIdx = 0;

    while (true) {
        if (m_state.load() == PipelineState::Paused) {
            std::unique_lock<std::mutex> lk(m_pauseMutex);
            m_pauseCv.wait(lk, [this] { return m_state.load() != PipelineState::Paused; });
        }
        if (m_state.load() != PipelineState::Running) {
            LogMsg("PIP: ","Decode: state not Running, exiting");
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
        slot.seq = decFrameIdx;

        bool useGPU = m_decoder.IsHWDecoding();
        if (useGPU) {
            const uint8_t* yDev = nullptr;
            const uint8_t* uvDev = nullptr;
            int yPitch = 0, uvPitch = 0;
            if (!m_decoder.ReadFrameGPU(&yDev, &yPitch, &uvDev, &uvPitch, &slot.pts)) {
                LogMsg("PIP: ","Decode GPU: EOF or error");
                slot.state.store(SlotState::Empty);
                m_decodeDone.store(true);
                m_slotCv.notify_one();
                break;
            }

            // Synchronise NVDEC default-stream output with the per-slot non-blocking
            // stream.  cudaStreamNonBlocking does NOT implicitly synchronise with
            // stream 0, so we use an inter-stream event barrier to guarantee the
            // NVDEC decoded surface is fully written before nv12_to_rgba reads it.
            cudaEventRecord(slot.decodeEvent, 0);
            cudaStreamWaitEvent(slot.stream, slot.decodeEvent, 0);

            launch_nv12_to_rgba(
                yDev, yPitch,
                uvDev, uvPitch,
                slot.d_rgba_src, m_srcW * 4,
                m_srcW, m_srcH, slot.stream, m_colorMatrix);
        } else {
            int stride = m_srcW;
            if (!m_decoder.ReadFrameNV12(slot.nv12_cpu, &stride)) {
                LogMsg("PIP: ","Decode CPU: EOF or error");
                slot.state.store(SlotState::Empty);
                m_decodeDone.store(true);
                m_slotCv.notify_one();
                break;
            }
            slot.pts = m_decoder.GetLastPTS();

            // H2D copy CPU NV12 → GPU d_nv12
            size_t nv12Size = (size_t)m_srcW * m_srcH * 3 / 2;
            cudaMemcpyAsync(slot.d_nv12, slot.nv12_cpu, nv12Size,
                            cudaMemcpyHostToDevice, slot.stream);

            launch_nv12_to_rgba(
                slot.d_nv12, stride,
                slot.d_nv12 + m_srcW * m_srcH, stride,
                slot.d_rgba_src, m_srcW * 4,
                m_srcW, m_srcH, slot.stream, m_colorMatrix);
        }

        cudaStreamSynchronize(slot.stream);
        if (CudaFailed("Decode: NV12->RGBA")) {
            LogMsg("PIP: ","Decode: CUDA error in NV12->RGBA");
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
            LogMsg("PIP: ",buf);
        }
    }

    LogMsg("PIP: ","--- Decode thread finished ---");
}

static void SafeCleanup(VideoEncoder& enc, VSRProcessor& vsr, VideoDecoder& dec) {
    __try { enc.Close(); } __except (EXCEPTION_EXECUTE_HANDLER) { LogMsg("PIP: ","SEH in encoder.Close()"); }
    // VSR shutdown is intentionally NOT called between pipeline runs.
    // NGX does not support clean re-initialisation in the same process —
    // calling rtx_video_api_cuda_shutdown() followed by a second
    // rtx_video_api_cuda_create() can produce a crash on the first
    // ProcessFrame (access violation in NGX driver internals).
    // Real NGX shutdown is deferred to VSRProcessor::GlobalShutdown()
    // which runs at process exit.
    __try { dec.Close(); } __except (EXCEPTION_EXECUTE_HANDLER) { LogMsg("PIP: ","SEH in decoder.Close()"); }
}

void PipelineController::ThreadFuncImpl() {
    std::chrono::time_point<std::chrono::high_resolution_clock> lastReport;
    LogMsg("PIP: ","--- Pipeline GPU thread started ---");

    if (m_state.load() != PipelineState::Starting) {
        LogMsg("PIP: ","State not Starting, aborting");
        return;
    }

    // ---- CUDA runtime init ----
    LogStatus(onStatus, "CUDA 运行时初始化...");
    {
        cudaError_t initErr = cudaSetDevice(m_cfg.gpuIndex);
        if (initErr != cudaSuccess) {
            LogMsg("PIP: ","cudaSetDevice failed");
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
    if (!m_decoder.Open(m_cfg.inputPath.c_str(), &info, true)) {
        LogMsg("PIP: ","Failed to open input file");
        if (onError) onError(L"无法打开输入文件");
        m_state.store(PipelineState::Error);
        return;
    }
    LogStatus(onStatus, "解码器打开成功");
    if (m_decoder.IsHWDecoding()) {
        LogStatus(onStatus, "GPU 硬件解码已启用 (NVDEC)");
        strncpy(m_decodeMode, "GPU", sizeof(m_decodeMode) - 1);
    } else {
        LogStatus(onStatus, "NVDEC 不可用，使用软件解码 (CPU)");
        strncpy(m_decodeMode, "CPU", sizeof(m_decodeMode) - 1);
    }

    m_srcW = info.width;
    m_srcH = info.height;
    m_srcFps = info.fps;
    m_totalFrames = info.totalFrames;
    if (m_totalFrames <= 0) m_totalFrames = 1;
    m_srcTimeBaseNum = info.srcTimeBaseNum;
    m_srcTimeBaseDen = info.srcTimeBaseDen;

    // Store pipeline flags
    m_trueHdrEnabled = m_cfg.trueHdrEnabled;

    // Store source colour metadata
    m_colorMatrix       = info.srcColorMatrix;
    m_avColorPrimaries  = info.avColorPrimaries;
    m_avColorTransfer  = info.avColorTransfer;
    m_avColorSpace     = info.avColorSpace;
    m_avColorRange     = info.avColorRange;

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Source: %dx%d fps=%.3f frames=%d tb=%d/%d codec=%s audio=%s",
            info.width, info.height, info.fps, info.totalFrames,
            info.srcTimeBaseNum, info.srcTimeBaseDen,
            info.videoCodecName, info.hasAudio ? info.audioCodecName : "none");
        LogMsg("PIP: ",buf);
        { char _b[128]; snprintf(_b, sizeof(_b),
            "PIP: color: avCS=%d avRange=%d → matrix=%d",
            info.avColorSpace, info.avColorRange, m_colorMatrix);
          LogMsg("PIP: ",_b); }
    }

    CalculateOutputSize(m_srcW, m_srcH, m_dstW, m_dstH);
    double outFps = m_cfg.outputFps > 0 ? (double)m_cfg.outputFps : m_srcFps;

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Output: %dx%d fps=%.3f quality=%d encoder=%d crf=%d speed=%d",
            m_dstW, m_dstH, outFps, m_cfg.qualityLevel, m_cfg.encoderIndex, m_cfg.crf, m_cfg.encoderSpeed);
        LogMsg("PIP: ",buf);
    }

    // ---- Allocate frame slots (pinned CPU memory + GPU + per-slot streams) ----
    LogStatus(onStatus, "分配帧缓冲区...");
    size_t nv12Size     = (size_t)m_srcW * m_srcH * 3 / 2;
    // P010 output is w*h*4 bytes vs NV12's w*h*3/2 (10-bit uses 16-bit per sample)
    size_t outSize      = m_trueHdrEnabled
        ? (size_t)m_dstW * m_dstH * 3
        : (size_t)m_dstW * m_dstH * 3 / 2;
    size_t rgbaSrcSize  = (size_t)m_srcW * m_srcH * 4;
    size_t rgbaDstSize  = (size_t)m_dstW * m_dstH * 4;

    { char buf[256]; snprintf(buf, sizeof(buf), "src=%dx%d dst=%dx%d out=%zu rgba=%zu hdr=%d",
        m_srcW, m_srcH, m_dstW, m_dstH, outSize, rgbaSrcSize, m_trueHdrEnabled); LogMsg("PIP: ",buf); }

    for (int i = 0; i < NUM_SLOTS; i++) {
        m_slots[i].w = m_srcW;
        m_slots[i].h = m_srcH;
        m_slots[i].dstW = m_dstW;
        m_slots[i].dstH = m_dstH;

        // Pinned (page-locked) CPU memory for high-speed DMA transfers
        if (cudaMallocHost(&m_slots[i].nv12_cpu, nv12Size) != cudaSuccess ||
            cudaMallocHost(&m_slots[i].nv12_out_cpu, outSize) != cudaSuccess) {
            LogMsg("PIP: ","Failed to allocate pinned CPU memory");
            if (onError) onError(L"内存分配失败");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }

        if (cudaMalloc(&m_slots[i].d_nv12, nv12Size) != cudaSuccess ||
            cudaMalloc(&m_slots[i].d_rgba_src, rgbaSrcSize) != cudaSuccess ||
            cudaMalloc(&m_slots[i].d_rgba_dst, rgbaDstSize) != cudaSuccess ||
            cudaMalloc(&m_slots[i].d_nv12_out, outSize) != cudaSuccess) {
            LogMsg("PIP: ","Failed to allocate GPU memory");
            if (onError) onError(L"GPU 内存分配失败，请检查显存");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }

        // Non-blocking per-slot streams for GPU kernel overlap
        if (cudaStreamCreateWithFlags(&m_slots[i].stream, cudaStreamNonBlocking) != cudaSuccess) {
            LogMsg("PIP: ","Failed to create CUDA stream");
            m_state.store(PipelineState::Error);
            goto cleanup;
        }

        // Inter-stream event for NVDEC default-stream → per-slot stream sync
        if (cudaEventCreate(&m_slots[i].decodeEvent) != cudaSuccess) {
            LogMsg("PIP: ","Failed to create CUDA event");
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
    if (!SafeVSRInit(&m_vsr, m_cfg.gpuIndex, m_trueHdrEnabled)) {
        LogMsg("PIP: ","VSR init failed");
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

        encCfg.colorPrimaries = m_avColorPrimaries;
        encCfg.colorTransfer  = m_avColorTransfer;
        encCfg.colorSpace     = m_avColorSpace;
        encCfg.colorRange     = m_avColorRange;
        encCfg.use10Bit       = m_trueHdrEnabled;

        std::string lastEncErr;
        auto encStatus = [&](const char* msg) {
            LogMsg("PIP: ",msg);
            if (msg) lastEncErr = msg;
            if (onStatus) onStatus(msg);
        };
        if (!m_encoder.Open(encCfg, encStatus)) {
            LogMsg("PIP: ","Failed to open encoder");
            if (onError) {
                wchar_t wbuf[512];
                if (!lastEncErr.empty())
                    MultiByteToWideChar(CP_UTF8, 0, lastEncErr.c_str(), -1, wbuf, 512);
                else
                    wcscpy(wbuf, L"无法打开编码器");
                onError(wbuf);
            }
            m_state.store(PipelineState::Error);
            goto cleanup;
        }
    }
    LogStatus(onStatus, "编码器打开成功");

    // ---- Launch decode thread ----
    m_decodeDone.store(false);
    m_framesEncoded.store(0);
    LogMsg("PIP: ","Step: Launching decode thread...");
    m_decodeThread = std::thread(&PipelineController::DecodeFunc, this);
    LogMsg("PIP: ","Decode thread launched");

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
            m_pauseCv.wait(lk, [this] { return m_state.load() != PipelineState::Paused; });
        }
        if (m_state.load() != PipelineState::Running) {
            LogMsg("PIP: ","GPU: state not Running, exiting loop");
            break;
        }

        // Wait for any VSR_Ready slot.  Frames arrive from the decoder in
        // display order (PTS-sorted by the decoder's internal reorder buffer),
        // so any ready slot with the smallest seq number contains the next
        // display-ordered frame.
        {
            std::unique_lock<std::mutex> lk(m_slotMutex);
            m_slotCv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                for (int i = 0; i < NUM_SLOTS; i++)
                    if (m_slots[i].state.load() == SlotState::VSR_Ready) return true;
                return m_decodeDone.load();
            });
        }

        // Pick the VSR_Ready slot with the smallest seq number.
        // Seq is assigned monotonically by the decode thread in display order
        // (guaranteed by the decoder's internal PTS reorder buffer).
        int slotIdx = -1;
        int bestSeq = INT_MAX;
        for (int i = 0; i < NUM_SLOTS; i++) {
            SlotState s = m_slots[i].state.load(std::memory_order_acquire);
            if (s != SlotState::VSR_Ready) continue;
            int seq = m_slots[i].seq.load();
            if (slotIdx < 0 || seq < bestSeq) {
                bestSeq = seq;
                slotIdx = i;
            }
        }
        if (slotIdx >= 0) {
            SlotState expected = SlotState::VSR_Ready;
            if (!m_slots[slotIdx].state.compare_exchange_strong(expected, SlotState::Encoding)) {
                slotIdx = -1;
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
                    LogMsg("PIP: ","GPU: all slots done, exiting loop");
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
            LogMsg("PIP: ","VSR evaluate failed or crashed");
            if (onError) onError(L"VSR 处理失败");
            m_state.store(PipelineState::Error);
            break;
        }

        // ---- Sync VSR output (default stream) → per-slot stream ----
        // VSR/NGX runs on the default CUDA stream (stream 0) and writes
        // d_rgba_dst asynchronously.  The rgba_to_nv12 kernel below runs
        // on the per-slot non-blocking stream (cudaStreamNonBlocking).
        // cudaStreamNonBlocking does NOT participate in default-stream
        // implicit synchronisation, so without an explicit barrier the
        // conversion kernel may read stale / partially-written data and
        // produce corrupted output (old frame content leaking through).
        cudaEventRecord(slot.decodeEvent, 0);          // VSR's default stream
        cudaStreamWaitEvent(slot.stream, slot.decodeEvent, 0);  // per-slot waits

        // Encode: try GPU zero-copy first, fall back to D2H + CPU encode
        {
            int64_t encPts = m_framesEncoded.load();

            // Try NVENC zero-copy: get CUDA hwframe buffer, render directly into it
            uint8_t* encY = nullptr;
            uint8_t* encUV = nullptr;
            int encYPitch = 0, encUVPitch = 0;
            if (m_encoder.GetFrameBuffer(&encY, &encYPitch, &encUV, &encUVPitch)) {
                // GPU zero-copy path: render directly into encoder's CUDA hwframe
                if (m_trueHdrEnabled) {
                    launch_abgr10_to_p010(
                        slot.d_rgba_dst, m_dstW * 4,
                        encY, encYPitch,
                        encUV, encUVPitch,
                        m_dstW, m_dstH, true, slot.stream);
                } else {
                    launch_rgba_to_nv12(
                        slot.d_rgba_dst, m_dstW * 4,
                        encY, encYPitch,
                        encUV, encUVPitch,
                        m_dstW, m_dstH, slot.stream, m_colorMatrix);
                }
                cudaStreamSynchronize(slot.stream);
                if (CudaFailed(m_trueHdrEnabled ? "GPU: ABGR10->P010 (CUDA enc)" : "GPU: RGBA->NV12 (CUDA enc)")) {
                    m_state.store(PipelineState::Error);
                    break;
                }
                if (!m_encoder.SubmitFrame(encPts)) {
                    LogMsg("PIP: ","Encode failed (CUDA)");
                    if (onError) onError(L"编码失败 (CUDA)");
                    m_state.store(PipelineState::Error);
                    break;
                }
            } else {
                // CPU fallback: render to output buffer, D2H, CPU encode
                int outYStride  = m_trueHdrEnabled ? m_dstW * 2 : m_dstW;
                int outUVStride = m_trueHdrEnabled ? m_dstW * 2 : m_dstW;
                size_t yPlaneBytes = outYStride * m_dstH;
                if (m_trueHdrEnabled) {
                    launch_abgr10_to_p010(
                        slot.d_rgba_dst, m_dstW * 4,
                        slot.d_nv12_out, outYStride,
                        slot.d_nv12_out + yPlaneBytes, outUVStride,
                        m_dstW, m_dstH, true, slot.stream);
                } else {
                    launch_rgba_to_nv12(
                        slot.d_rgba_dst, m_dstW * 4,
                        slot.d_nv12_out, outYStride,
                        slot.d_nv12_out + yPlaneBytes, outUVStride,
                        m_dstW, m_dstH, slot.stream, m_colorMatrix);
                }
                cudaMemcpyAsync(slot.nv12_out_cpu, slot.d_nv12_out, outSize,
                                cudaMemcpyDeviceToHost, slot.stream);
                cudaStreamSynchronize(slot.stream);
                if (CudaFailed(m_trueHdrEnabled ? "GPU: ABGR10->P010 + D2H" : "GPU: RGBA->NV12 + D2H")) {
                    LogMsg("PIP: ","GPU: CUDA error in convert kernel or D2H");
                    m_state.store(PipelineState::Error);
                    break;
                }
                if (!m_encoder.WriteFrameNV12(slot.nv12_out_cpu, outYStride, outUVStride, encPts)) {
                    LogMsg("PIP: ","Encode failed");
                    if (onError) onError(L"编码失败");
                    m_state.store(PipelineState::Error);
                    break;
                }
            }

            if (encPts % 500 == 0) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "GPU: frame %lld (slot %d) pts=%lld seq=%d state=%d",
                    (long long)encPts, slotIdx,
                    (long long)slot.pts, slot.seq.load(),
                    (int)slot.state.load());
                LogMsg("PIP: ",buf);
            }
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
            strncpy(p.decodeMode, m_decodeMode, sizeof(p.decodeMode) - 1);
            onProgress(p);
        }
        lastReport = now;

        if (encoded % 100 == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Encoded %d frames so far", encoded);
            LogMsg("PIP: ",buf);
        }
    }

    { char buf[128]; snprintf(buf, sizeof(buf), "GPU loop ended, total frames encoded: %d",
        m_framesEncoded.load()); LogMsg("PIP: ",buf); }

    // ---- Wait for decode thread ----
    LogMsg("PIP: ","Waiting for decode thread to join...");
    if (m_decodeThread.joinable()) {
        m_decodeDone.store(true);
        m_slotCv.notify_all();
        m_decodeThread.join();
    }
    LogMsg("PIP: ","Decode thread joined");

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
        if (m_slots[i].decodeEvent) { cudaEventDestroy(m_slots[i].decodeEvent); m_slots[i].decodeEvent = nullptr; }
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
    LogMsg("PIP: ","Pipeline GPU thread finished");
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
    LogMsg("PIP: ","Pipeline thread started");
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
