# RTX VSR Tool

基于 NVIDIA RTX Video Super Resolution (VSR) 的 Windows 视频 AI 升频工具。使用 Win32 API 构建界面，FFmpeg 进行编解码，CUDA 完成色彩转换和 NGX 推理。

## 功能

- **AI 升频**：支持 2x、4x 或自定义分辨率放大
- **质量等级**：Bicubic / Low / Medium / High / Ultra
- **硬件编码**：NVENC H.264、HEVC、AV1 + 软件编码器（libx264、libx265、libaom-av1）
- **音频处理**：AAC 转码或直接复用（remux）源音频
- **多 GPU 支持**：可选指定 GPU
- **编码控制**：CRF 质量、速度预设、封装格式（mp4/mkv/mov）
- **SEH 异常保护**：管线全流程和 NGX 调用均以结构化异常处理保护，防止意外崩溃
- **INI 配置持久化**：设置保存于可执行文件同目录下

## 硬件要求

- **GPU**：NVIDIA Turing（RTX 20 系列）及以上，驱动 550+
- **Visual Studio 2022**：需安装"使用 C++ 的桌面开发"和"CUDA"工作负载
- **CMake** 3.20+
- **CUDA Toolkit** 12.x

## 依赖项

| 依赖 | 位置 |
|---|---|
| FFmpeg 8.1.1 shared | [`ffmpeg-8.1.1-full_build-shared/`](../ffmpeg-8.1.1-full_build-shared/) |
| NVIDIA RTX Video SDK v1.1.0 | [`RTX_Video_SDK_v1.1.0/`](../RTX_Video_SDK_v1.1.0/) |
| NVIDIA DLSS SDK 310.6 (NGX Core) | [`DLSS-310.6.0/`](../DLSS-310.6.0/) |

构建脚本期望以下目录与 `RTX_VSR_CMake/` 平级：

```
RTX_VSR/
├── RTX_VSR_CMake/              # 本仓库
├── ffmpeg-8.1.1-full_build-shared/
├── RTX_Video_SDK_v1.1.0/
└── DLSS-310.6.0/
```

## 构建

### 快速构建（批处理）

```cmd
build.bat [Debug|Release]
```

### 快速构建（PowerShell）

```powershell
build.ps1 -Config Release
build.ps1 -Config Debug -Fresh        # 重新 CMake 配置
build.ps1 -Config Debug -Clean        # 清理后重新构建
```

### 手动 CMake

```bash
cd RTX_VSR_CMake
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_CUDA_ARCHITECTURES="75;86;89"
cmake --build build --config Release
```

输出位于 `build/Release/RTX_VSR_Tool.exe`。

### 独立 NGX 初始化测试（诊断 GPU/驱动 VSR 支持）

```bash
cd RTX_VSR_CMake
cmake --build build --config Release --target ngx_init_test
./build/Release/ngx_init_test.exe
```

## 使用方法

1. 启动 `RTX_VSR_Tool.exe`
2. 选择输入视频（也支持拖放）
3. 设置输出路径和编码参数
4. 选择升频模式：2x、4x 或自定义
5. 设置 VSR 质量等级
6. 点击 **Start**

进度面板实时显示 FPS、剩余时间和单帧耗时。管线支持暂停与恢复。

## 架构

### 线程模型

- **主线程**：Win32 消息循环，通过回调接收进度更新
- **解码线程**：FFmpeg 解码 → H2D 传输 → CUDA NV12→RGBA 转换，完成后将槽位标记为 `VSR_Ready`
- **GPU 线程**：消费 `VSR_Ready` 槽位，执行 NGX VSR 推理 → RGBA→NV12 转换 → D2H 传输 → FFmpeg 编码
- **状态机**：`Idle → Starting → Running ↔ Paused → Completed/Error`，`std::atomic` + `std::condition_variable` 实现
- **槽位同步**：每个 `FrameSlot` 具有 `std::atomic<SlotState>` 状态（`Empty → Decoding → VSR_Ready → Encoding → Empty`），CAS 操作实现无锁获取，`condition_variable` 实现线程间通知

### 数据处理管线

解码线程（逐帧循环）：
```
FFmpeg 解码器（CPU 上输出 NV12）
    ↓ cudaMemcpyAsync H2D（per-slot non-blocking stream）
CUDA 核函数: nv12_to_rgba（BT.709 色彩空间）
    ↓ slot.state → VSR_Ready（通知 GPU 线程）
```

GPU 线程（消费 VSR_Ready 槽位）：
```
NGX VSR 推理
    ↓
CUDA 核函数: rgba_to_nv12（BT.709 色彩空间）
    ↓ cudaMemcpyAsync D2H（per-slot non-blocking stream）
FFmpeg 编码器（NV12 → H.264/HEVC/AV1）
    ↓ slot.state → Empty（通知解码线程）
```

两个线程通过 3 个 FrameSlot 的原子状态同步，解码第 N+1 帧与编码第 N-1 帧可重叠执行。

### 3-Slot 帧流水线

三个 `FrameSlot` 循环使用，每个 Slot 在管线启动时一次性分配 GPU 显存和固定页面主机内存（`cudaMallocHost`，提升 DMA 传输带宽 2-3x），每个 Slot 拥有独立的 Non-Blocking CUDA Stream 以支持 GPU 内核并发执行。

```
时序示例（稳定运行后）:

时间 →  Slot 0                  Slot 1                  Slot 2
        ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
        │ Decode(N)       │     │ Decode(N+1)     │     │ Decode(N+2)     │
        │ H2D + YUV→RGB   │     │ H2D + YUV→RGB   │     │ H2D + YUV→RGB   │
        │                 │     │                 │     │                 │
        │ VSR(N)          │     │ VSR(N+1)        │     │ VSR(N+2)        │
        │ RGB→YUV + D2H   │     │ RGB→YUV + D2H   │     │ RGB→YUV + D2H   │
        │ Encode(N)       │     │ Encode(N+1)     │     │ Encode(N+2)     │
        └─────────────────┘     └─────────────────┘     └─────────────────┘
```

### 项目结构

```
src/
├── main.cpp                 # WinMain、消息循环、SEH 异常保护
├── main_window.cpp/h        # Win32 界面、文件对话框、进度显示
├── pipeline_ctrl.cpp/h      # PipelineController — 解码线程 + GPU 线程、3-Slot 帧状态机
├── vsr_processor.cpp/h      # VSRProcessor — NGX CUDA 初始化/推理/关闭
├── rtx_video_api_cuda_impl.cpp  # NGX CUDA 实现（NVIDIA 示例代码）
├── video_decoder.cpp/h      # FFmpeg 解码 → NV12、音频包队列
├── video_encoder.cpp/h      # FFmpeg NVENC/软件编码、音频转码/复用
├── cuda_yuv.cu              # 自定义 CUDA 核函数：NV12↔RGBA 转换
├── ngx_init_test.cpp        # 独立 NGX 初始化诊断工具
├── config.cpp/h             # INI 配置读写
└── include/                 # NGX SDK 头文件、utils.h
```

## 输出格式

- **封装**：mp4（默认）、mkv、mov — 根据输出文件扩展名自动识别
- **编码器**：6 种可选 — NVENC H.264/HEVC/AV1 或软件 libx264/libx265/libaom-av1
- **尺寸对齐**：16 像素对齐，确保 NV12 色度平面兼容
- **像素格式**：解码→编码全链路 NV12；GPU 上仅 VSR 阶段为 RGBA

## 编码速度预设

| 速度 | NVENC | 软件编码器 |
|---|---|---|
| 0（最快） | p1 (fastest) | ultrafast |
| 1 | p3 | superfast |
| 2（默认） | p4 (balanced) | veryfast |
| 3 | p6 | medium |
| 4（最慢） | p7 (quality) | veryslow |

## 已知限制

- TrueHDR（SDR→HDR 转换）SDK 已支持，但尚未接入 UI
- 当前仅输出 8-bit（10-bit 需修改编码器和 CUDA 核函数）
- 仅支持单 GPU 处理
- 仅支持本地文件，不支持流媒体/URL
