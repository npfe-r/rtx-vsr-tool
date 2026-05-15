#include <cuda_runtime.h>
#include <cstdint>

static __device__ __forceinline__ int clamp(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// NV12 -> RGBA (BT.601 Limited Range)
// Each thread handles one output pixel
// y_plane: w x h bytes, y_pitch = stride of Y plane (>= w)
// uv_plane: w x (h/2) bytes interleaved U/V, uv_pitch = stride of UV plane (>= w)
// rgba_out: w x h x 4 bytes, rgba_pitch = stride of RGBA (>= w*4)
__global__ void nv12_to_rgba_kernel(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    int Y = y_plane[y * y_pitch + x] - 16;
    int U = uv_plane[(y / 2) * uv_pitch + (x & ~1)] - 128;
    int V = uv_plane[(y / 2) * uv_pitch + (x & ~1) + 1] - 128;

    // BT.601 limited range integer matrix
    int r = (298 * Y           + 409 * V + 128) >> 8;
    int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
    int b = (298 * Y + 516 * U           + 128) >> 8;

    int out_idx = y * rgba_pitch + x * 4;
    rgba_out[out_idx + 0] = (uint8_t)clamp(r, 0, 255);
    rgba_out[out_idx + 1] = (uint8_t)clamp(g, 0, 255);
    rgba_out[out_idx + 2] = (uint8_t)clamp(b, 0, 255);
    rgba_out[out_idx + 3] = 0xFF;
}

// RGBA -> NV12 (BT.601 Limited Range)
// Each thread handles one pixel for Y, UV computed once per 2x2 block
__global__ void rgba_to_nv12_kernel(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    int idx = y * rgba_pitch + x * 4;
    int R = rgba[idx + 0];
    int G = rgba[idx + 1];
    int B = rgba[idx + 2];

    // Y
    int Y_val = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
    y_plane[y * y_pitch + x] = (uint8_t)clamp(Y_val, 0, 255);

    // UV - each 2x2 block computes once (only when thread covers top-left of block)
    if ((x & 1) == 0 && (y & 1) == 0) {
        int sumR = 0, sumG = 0, sumB = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int i = (y + dy) * rgba_pitch + (x + dx) * 4;
                sumR += rgba[i + 0];
                sumG += rgba[i + 1];
                sumB += rgba[i + 2];
            }
        }
        int avgR = sumR / 4;
        int avgG = sumG / 4;
        int avgB = sumB / 4;
        int U_val = ((-38 * avgR - 74 * avgG + 112 * avgB + 128) >> 8) + 128;
        int V_val = ((112 * avgR - 94 * avgG - 18 * avgB + 128) >> 8) + 128;
        int uv_idx = (y / 2) * uv_pitch + x;
        uv_plane[uv_idx]     = (uint8_t)clamp(U_val, 0, 255);
        uv_plane[uv_idx + 1] = (uint8_t)clamp(V_val, 0, 255);
    }
}

// Host-callable wrappers

extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream)
{
    dim3 block(16, 16);
    dim3 grid((w + 15) / 16, (h + 15) / 16);
    nv12_to_rgba_kernel<<<grid, block, 0, stream>>>(
        y_plane, y_pitch, uv_plane, uv_pitch, rgba_out, rgba_pitch, w, h);
}

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream)
{
    dim3 block(16, 16);
    dim3 grid((w + 15) / 16, (h + 15) / 16);
    rgba_to_nv12_kernel<<<grid, block, 0, stream>>>(
        rgba, rgba_pitch, y_plane, y_pitch, uv_plane, uv_pitch, w, h);
}
