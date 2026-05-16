#include <cuda_runtime.h>
#include <cstdint>
#include "color_types.h"

// ── Colour-matrix coefficient tables (device-accessible) ────────────
// Forward RGB→YUV (limited range):
//   Y = (yr*R + yg*G + yb*B + 128) >> 8 + yOff
//   U = (cbr*R + cbg*G + cbb*B + 128) >> 8 + 128
//   V = (crr*R + crg*G + crb*B + 128) >> 8 + 128
// ──────────────────────────────────────────────────────────
#define FWD(YR,YG,YB, CBR,CBG,CBB, CRR,CRG,CRB, YO) \
    { YR, YG, YB, CBR, CBG, CBB, CRR, CRG, CRB, YO }
__device__ static const int RGB2YUV[3][10] = {
    FWD( 66, 129,  25,  -38, -74, 112,  112, -94, -18,  16 ),  // BT.601
    FWD( 47, 157,  16,  -26, -87, 112,  112,-102, -10,  16 ),  // BT.709
    FWD( 58, 149,  13,  -31, -81, 112,  112,-103,  -9,  16 ),  // BT.2020
};
#undef FWD

// Inverse YUV→RGB (limited range):
//   Y' = Y - yOff,  U' = U - 128,  V' = V - 128
//   R = (yCoeff*Y'          + rv*V' + 128) >> 8
//   G = (yCoeff*Y' - gu*U' - gv*V' + 128) >> 8
//   B = (yCoeff*Y' + bu*U'          + 128) >> 8
// ──────────────────────────────────────────────────────────
__device__ static const int YUV2RGB[3][6] = {
    { 298, 409, 100, 208, 516, 16 },  // BT.601
    { 298, 459,  55, 136, 541, 16 },  // BT.709
    { 298, 430,  48, 167, 549, 16 },  // BT.2020
};

static __device__ __forceinline__ int clamp(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// ────────────────────────────────────────────────────────────────────
// NV12 → RGBA  (BT.601 / BT.709 / BT.2020 Limited Range)
// ────────────────────────────────────────────────────────────────────
// Each thread handles one output pixel.
// y_plane:  w × h bytes,  y_pitch = stride of Y plane (≥ w)
// uv_plane: w × (h/2) bytes interleaved U/V,  uv_pitch = stride (≥ w)
// rgba_out: w × h × 4 bytes,  rgba_pitch = stride of RGBA (≥ w*4)
__global__ void nv12_to_rgba_kernel(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, int matrix)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const int* c = YUV2RGB[matrix];
    int Y = y_plane[y * y_pitch + x] - c[5];  // yOff
    int U = uv_plane[(y / 2) * uv_pitch + (x & ~1)] - 128;
    int V = uv_plane[(y / 2) * uv_pitch + (x & ~1) + 1] - 128;

    int r = (c[0] * Y       + c[1] * V + 128) >> 8;  // yCoeff, rv
    int g = (c[0] * Y - c[2] * U - c[3] * V + 128) >> 8;  // gu, gv
    int b = (c[0] * Y + c[4] * U       + 128) >> 8;  // bu

    int out_idx = y * rgba_pitch + x * 4;
    rgba_out[out_idx + 0] = (uint8_t)clamp(r, 0, 255);
    rgba_out[out_idx + 1] = (uint8_t)clamp(g, 0, 255);
    rgba_out[out_idx + 2] = (uint8_t)clamp(b, 0, 255);
    rgba_out[out_idx + 3] = 0xFF;
}

// ────────────────────────────────────────────────────────────────────
// RGBA → NV12  (BT.601 / BT.709 / BT.2020 Limited Range)
// ────────────────────────────────────────────────────────────────────
// Each thread handles one pixel for Y, and UV is computed once per 2×2 block.
__global__ void rgba_to_nv12_kernel(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, int matrix)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const int* c = RGB2YUV[matrix];

    int idx = y * rgba_pitch + x * 4;
    int R = rgba[idx + 0];
    int G = rgba[idx + 1];
    int B = rgba[idx + 2];

    // Y  (every pixel)
    int Y_val = ((c[0] * R + c[1] * G + c[2] * B + 128) >> 8) + c[9];  // yr, yg, yb, yOff
    y_plane[y * y_pitch + x] = (uint8_t)clamp(Y_val, 0, 255);

    // UV  (each 2×2 block once — top-left corner thread)
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
        int U_val = ((c[3] * avgR + c[4] * avgG + c[5] * avgB + 128) >> 8) + 128;  // cbr,cbg,cbb
        int V_val = ((c[6] * avgR + c[7] * avgG + c[8] * avgB + 128) >> 8) + 128;  // crr,crg,crb
        int uv_idx = (y / 2) * uv_pitch + x;
        uv_plane[uv_idx]     = (uint8_t)clamp(U_val, 0, 255);
        uv_plane[uv_idx + 1] = (uint8_t)clamp(V_val, 0, 255);
    }
}

// ────────────────────────────────────────────────────────────────────
// Host-callable wrappers
// ────────────────────────────────────────────────────────────────────

extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix)
{
    dim3 block(16, 16);
    dim3 grid((w + 15) / 16, (h + 15) / 16);
    nv12_to_rgba_kernel<<<grid, block, 0, stream>>>(
        y_plane, y_pitch, uv_plane, uv_pitch,
        rgba_out, rgba_pitch, w, h, colorMatrix);
}

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix)
{
    dim3 block(16, 16);
    dim3 grid((w + 15) / 16, (h + 15) / 16);
    rgba_to_nv12_kernel<<<grid, block, 0, stream>>>(
        rgba, rgba_pitch,
        y_plane, y_pitch, uv_plane, uv_pitch,
        w, h, colorMatrix);
}
