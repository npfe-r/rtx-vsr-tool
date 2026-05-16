# RTX VSR Tool

基于 NVIDIA RTX Video Super Resolution (VSR) 的 Windows 桌面 GUI 工具，利用 AI 对视频进行超分辨率放大。使用 Dear ImGui (D3D11) 构建界面，FFmpeg 进行编解码，CUDA 完成色彩空间转换与 NGX 推理。

## 功能

- **AI 升频**：支持 2x、4x 或自定义输出分辨率
- **质量等级**：Bicubic（不使用 VSR）/ Low / Medium / High / Ultra
- **硬件编码**：NVENC H.264、HEVC、AV1
- **软件编码**：libx264、libx265、libaom-av1
- **音频处理**：AAC 转码（默认 128kbps）或直接复制（remux）源音频，也支持输出无音频
- **多 GPU 支持**：可手动指定使用的 GPU 设备索引
- **编码控制**：CRF 质量参数、速度预设、封装格式（mp4/mkv/mov）
- **SEH 异常保护**：管线全流程和 NGX 调用均以结构化异常处理（SEH）保护，防止意外崩溃导致进程退出
- **INI 配置持久化**：窗口位置、上次使用的路径、编码参数等设置自动保存/恢复
- **拖放支持**：可直接将视频文件拖入窗口打开

## 硬件要求

- **GPU**：NVIDIA Turing（RTX 20 系列）及以上，含 Ampere（RTX 30）、Ada Lovelace（RTX 40）、Blackwell（RTX 50）
- **驱动版本**：NVIDIA 显卡驱动 550+
- **CUDA 架构**：SM 75（Turing）、SM 86（Ampere）、SM 89（Ada）、SM 100（Blackwell）
- **Visual Studio 2022**：需安装"使用 C++ 的桌面开发"和"CUDA"工作负载
- **CMake** 3.20+
- **CUDA Toolkit** 12.x

## 依赖项

项目依赖以下第三方 SDK，它们与 `RTX_VSR_CMake/` 平级放置：

| 依赖 | 路径 | 作用 |
|---|---|---|
| FFmpeg 8.1.1 shared | `../ffmpeg-8.1.1-full_build-shared/` | 视频解码/编码、音频处理 |
| NVIDIA RTX Video SDK v1.1.0 | `../RTX_Video_SDK_v1.1.0/` | VSR API 头文件和运行时 DLL（`nvngx_vsr.dll`、`nvngx_truehdr.dll`） |
| NVIDIA DLSS SDK 310.6 (NGX Core) | `../DLSS-310.6.0/` | NGX 核心库（`nvsdk_ngx_d.lib`） |
| Dear ImGui v1.91.0 | 通过 CMake FetchContent 从镜像拉取 | GUI 界面（含 Win32 + D3D11 后端） |

### 目录布局

```
Project Root/
├── RTX_VSR_CMake/                    # 本仓库 —— 源代码 + CMake 构建
│   ├── CMakeLists.txt                # 主构建文件
│   ├── build.bat                     # Windows CMD 构建脚本（项目级）
│   ├── build.ps1                     # PowerShell 构建脚本（项目级）
│   ├── cmake/                        # CMake 模块
│   │   ├── FindFFmpeg.cmake          # FFmpeg 查找模块（NO_DEFAULT_PATH）
│   │   └── imgui.cmake               # FetchContent 拉取 Dear ImGui
│   ├── include/                      # NGX SDK 头文件 + 工具头
│   ├── src/                          # 全部 C++ / CUDA 源代码
│   └── build/                        # 构建输出目录（.gitignore 忽略）
├── ffmpeg-8.1.1-full_build-shared/   # FFmpeg 共享库（.gitignore 忽略）
├── RTX_Video_SDK_v1.1.0/            # RTX Video SDK（.gitignore 忽略）
├── DLSS-310.6.0/                    # NGX Core（.gitignore 忽略）
├── build.bat                         # 根级构建快捷脚本
├── build.ps1                         # 根级构建快捷脚本
└── .gitignore
```

## 构建

### 快速构建（根级脚本）

从仓库根目录运行，会自动定位 `RTX_VSR_CMake/` 子目录：

```cmd
build.bat [Debug|Release]
```

```powershell
build.ps1 -Config Release
build.ps1 -Config Debug -Fresh        # 重新 CMake 配置（清除缓存）
build.ps1 -Config Debug -Clean        # 清理后重新构建
```

### 项目级构建

```cmd
cd RTX_VSR_CMake
build.bat [Debug|Release]
```

```powershell
cd RTX_VSR_CMake
.\build.ps1 -Config Release
```

### 手动 CMake

```bash
cd RTX_VSR_CMake
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_CUDA_ARCHITECTURES="75;86;89"
cmake --build build --config Release
```

> 注意：构建脚本（`build.bat` / `build.ps1`）额外传入了 `-DCMAKE_CUDA_ARCHITECTURES="75;86;89;100"` 以支持 Blackwell（RTX 50 系列）架构。手动 CMake 时如需 Blackwell 支持，请在配置命令中添加 `100`。

输出位于 `build/Release/RTX_VSR_Tool.exe`，构建系统通过 `POST_BUILD` 命令自动将必要的 DLL 复制到输出目录：
- `nvngx_vsr.dll`、`nvngx_truehdr.dll`（RTX Video SDK）
- `avcodec-62.dll`、`avformat-62.dll`、`avutil-60.dll`、`swscale-9.dll`、`swresample-6.dll`（FFmpeg）

### 独立 NGX 初始化诊断工具

用于快速检测 GPU/驱动是否支持 VSR：

```bash
cd RTX_VSR_CMake
cmake --build build --config Release --target ngx_init_test
./build/Release/ngx_init_test.exe
```

## 使用方法

1. 启动 `RTX_VSR_Tool.exe`
2. **选择输入**：点击顶部工具栏"选择文件"按钮，或直接将视频文件拖入窗口
3. **设置输出**：点击"输出路径"按钮指定保存位置（扩展名自动决定封装格式）
4. **配置参数**：
   - 左侧面板：显示输入视频信息（分辨率、帧率、编码、时长）和输出预计信息
   - 右侧面板：设置 VSR 质量等级、升频模式（2x/4x/自定义）、编码器、CRF、速度预设、音频模式、GPU 索引
5. 点击底栏 **开始** 启动处理管线

处理过程中：
- 进度条实时显示处理进度
- 状态文本居中显示当前帧数和 FPS
- 管线支持 **暂停** 与 **恢复**
- 窗口自动调整大小以适配所有设置项，无需滚动

## 架构

### 线程模型

采用 **双线程生产者-消费者** 流水线架构，通过 3 槽位环形缓冲（`FrameSlot`）和原子状态机协调：

| 线程 | 角色 | 工作内容 |
|---|---|---|
| 主线程 | UI | Win32 消息循环，通过回调接收进度更新 |
| 解码线程 | 生产者 | FFmpeg 解码 → `cudaMemcpyAsync` H2D → CUDA NV12→RGBA 转换 |
| GPU/编码线程 | 消费者 | NGX VSR 推理 → CUDA RGBA→NV12 转换 → `cudaMemcpyAsync` D2H → FFmpeg 编码 |

### 状态机

```
Idle → Starting → Running ↔ Paused → Completed / Error
```

- `std::atomic` + `std::condition_variable` 实现线程间同步
- 每个 `FrameSlot` 具有 `std::atomic<SlotState>` 状态，CAS 操作实现无锁获取

### 数据处理管线

```
解码线程（逐帧循环）：
  FFmpeg 解码器（CPU → NV12 帧）
    ↓ cudaMemcpyAsync H2D（per-slot non-blocking stream）
  CUDA 核函数: nv12_to_rgba（BT.709 色彩空间）
    ↓ slot.state → VSR_Ready（通知 GPU 线程）

GPU 线程（消费 VSR_Ready 槽位）：
  NGX VSR 推理
    ↓
  CUDA 核函数: rgba_to_nv12（BT.709 色彩空间）
    ↓ cudaMemcpyAsync D2H（per-slot non-blocking stream）
  FFmpeg 编码器（NV12 → H.264/HEVC/AV1）
    ↓ slot.state → Empty（通知解码线程）
```

两个线程通过 3 个 FrameSlot 的原子状态同步，解码第 N+1 帧与编码第 N-1 帧可重叠执行。

### 音频处理

音频在单独的路径中处理，与视频帧管线解耦：

- **无音频**：输出文件中不含音轨
- **复制（remux）**：直接从源文件中复用音频包，不重新编码，速度极快
- **转码**：使用 FFmpeg `swresample` 重采样后以 AAC 编码（默认 128kbps，可通过 CRF 参数控制）

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

### VSRProcessor（PIMPL 模式）

[VSRProcessor](file:///h:/Project/RTX_VSR/RTX_VSR_CMake/src/vsr_processor.h) 采用 PIMPL（Pointer to Implementation）设计模式，将 NGX SDK 的实现细节完全隐藏：

- **公开接口** (`vsr_processor.h`)：仅暴露 `Init()`、`Evaluate()`、`Shutdown()` 等纯逻辑接口
- **实现** (`vsr_processor.cpp`)：包含完整的 NGX CUDA 初始化、Feature Creation、推理调用链
- 调用者无需关心 NGX SDK 的头文件、初始化顺序或资源管理细节

### 项目结构

```
src/
├── main.cpp                      # WinMain、消息循环、SEH 异常保护
├── main_window.cpp / .h          # Win32 窗口管理、ImGui 界面、文件对话框、进度显示
├── pipeline_ctrl.cpp / .h        # PipelineController — 解码线程 + GPU 线程、3-Slot 帧状态机
├── vsr_processor.cpp / .h        # VSRProcessor — NGX CUDA 初始化/推理/关闭（PIMPL）
├── rtx_video_api_cuda_impl.cpp   # NGX CUDA 实现（基于 NVIDIA 官方示例）
├── video_decoder.cpp / .h        # FFmpeg 解码 → NV12 帧、音频包队列
├── video_encoder.cpp / .h        # FFmpeg 编码（NVENC/软件）、音频转码/复用
├── cuda_yuv.cu                   # 自定义 CUDA 核函数：NV12 ↔ RGBA 色彩空间转换
├── ngx_init_test.cpp             # 独立 NGX 初始化诊断工具（可单独构建）
├── test_cuda_enc.cpp             # CUDA hwcontext 诊断（已禁用编译）
├── config.cpp / .h               # INI 配置文件读写（基于 Windows GetPrivateProfileString）
└── include/
    ├── nvsdk_ngx*.h              # NGX SDK 头文件（14 个）
    ├── rtx_video_api.h           # RTX Video SDK 接口定义
    └── utils.h                   # 工具宏（APP_ID、SafeRelease 等）
```

## 输出格式

- **封装格式**：mp4（默认）、mkv、mov——根据输出文件扩展名自动识别
- **编码器**：6 种可选——NVENC H.264 / HEVC / AV1，软件 libx264 / libx265 / libaom-av1
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

## CMakeLists.txt 要点

项目主构建文件位于 [CMakeLists.txt](file:///h:/Project/RTX_VSR/RTX_VSR_CMake/CMakeLists.txt)：

- **项目名**：`RTX_VSR_Tool`，版本 1.0.0，语言 C++17 + CUDA
- **CUDA 架构**：默认 `75;86;89`（Turing / Ampere / Ada），构建脚本扩展了 `100`（Blackwell）
- **源文件收集**：使用 `file(GLOB_RECURSE src/*.cpp src/*.cu)`，添加新文件后需重新运行 cmake
- **源文件过滤**：`test_cuda_enc.cpp` 通过 `list(FILTER ... EXCLUDE REGEX)` 从主目标中排除，原为独立诊断目标，当前已禁用编译
- **CUDA 架构**：`CMakeLists.txt` 注释中写 Blackwell=120，但构建脚本实际传递 `100`——以构建脚本为准
- **Post-build 步骤**：自动复制 2 个 RTX SDK DLL + 5 个 FFmpeg DLL 到输出目录
- **MSVC 标志**：`/utf-8` 确保中文 UI 文本正确显示

## 已知限制

- TrueHDR（SDR→HDR 转换）SDK 已支持，但尚未接入 UI
- 当前仅输出 8-bit（10-bit 需修改编码器和 CUDA 核函数）
- 仅支持单 GPU 处理
- 仅支持本地文件，不支持流媒体 / URL
- CUDA 架构 100（Blackwell）在构建脚本中已启用，但 `CMakeLists.txt` 默认未包含——如遇 Blackwell GPU 请使用构建脚本或手动传入 `-DCMAKE_CUDA_ARCHITECTURES="75;86;89;100"`
