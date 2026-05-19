// cuda_rife.cu — CUDA 核函数用于 RIFE TensorRT 的格式转换
#include <cuda_runtime.h>
#include <cstdint>

// ── RGBA(8-bit packed) → RGB float (NCHW, [0,1]) ──
// RIFE ONNX 期望: NCHW float32, [0,1]
// 输入: rgba (H×W×4, byte, R=0 G=1 B=2 A=3)
// 输出: rgb_float (3×H×W, float32, channel-first)
//       对于 6ch 输入: 前 3ch=prev, 后 3ch=curr
//       offset_channels: 写入起始通道 (0 或 3)
__global__ void rgba_to_rgb_float_kernel(
    const uint8_t* __restrict__ rgba, int rgba_pitch,
    float* __restrict__ rgb_float, int w, int h, int offset_channels)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const uint8_t* src = rgba + y * rgba_pitch + x * 4;
    float inv = 1.0f / 255.0f;
    int base = offset_channels * w * h + y * w + x;

    rgb_float[base]               = src[0] * inv;  // R
    rgb_float[base + (size_t)w * h] = src[1] * inv;  // G
    rgb_float[base + (size_t)w * h * 2] = src[2] * inv;  // B
}

// ── RGB float (NCHW, [0,1]) → RGBA(8-bit packed) ──
__global__ void rgb_float_to_rgba_kernel(
    const float* __restrict__ rgb_float, int w, int h,
    uint8_t* __restrict__ rgba, int rgba_pitch)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    int base = y * w + x;
    float r = rgb_float[base];
    float g = rgb_float[base + (size_t)w * h];
    float b = rgb_float[base + (size_t)w * h * 2];

    uint8_t* dst = rgba + y * rgba_pitch + x * 4;
    dst[0] = (uint8_t)(min(max(r * 255.0f + 0.5f, 0.0f), 255.0f));
    dst[1] = (uint8_t)(min(max(g * 255.0f + 0.5f, 0.0f), 255.0f));
    dst[2] = (uint8_t)(min(max(b * 255.0f + 0.5f, 0.0f), 255.0f));
    dst[3] = 255;
}

// ── Launch wrappers (extern "C") ──
extern "C" void launch_rgba_to_rgb_float(
    const uint8_t* rgba, int rgba_pitch,
    float* rgb_float, int w, int h,
    int offset_channels, cudaStream_t stream)
{
    dim3 block(32, 16);
    dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
    rgba_to_rgb_float_kernel<<<grid, block, 0, stream>>>(
        rgba, rgba_pitch, rgb_float, w, h, offset_channels);
}

extern "C" void launch_rgb_float_to_rgba(
    const float* rgb_float, int w, int h,
    uint8_t* rgba, int rgba_pitch, cudaStream_t stream)
{
    dim3 block(32, 16);
    dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
    rgb_float_to_rgba_kernel<<<grid, block, 0, stream>>>(
        rgb_float, w, h, rgba, rgba_pitch);
}
