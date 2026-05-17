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
    rgba_out[out_idx + 0] = (uint8_t)clamp(r, 0, 255);    // R (CUDA tex: comp[0])
    rgba_out[out_idx + 1] = (uint8_t)clamp(g, 0, 255);    // G (comp[1])
    rgba_out[out_idx + 2] = (uint8_t)clamp(b, 0, 255);    // B (comp[2])
    rgba_out[out_idx + 3] = 0xFF;                          // A (comp[3])
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
    int R = rgba[idx + 0];   // CUDA tex: comp[0] = R
    int G = rgba[idx + 1];   // comp[1] = G
    int B = rgba[idx + 2];   // comp[2] = B

    // Y  (every pixel)
    int Y_val = ((c[0] * R + c[1] * G + c[2] * B + 128) >> 8) + c[9];  // yr, yg, yb, yOff
    y_plane[y * y_pitch + x] = (uint8_t)clamp(Y_val, 0, 255);

    // UV  (each 2×2 block once — top-left corner thread)
    if ((x & 1) == 0 && (y & 1) == 0) {
        int sumR = 0, sumG = 0, sumB = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int i = (y + dy) * rgba_pitch + (x + dx) * 4;
                sumR += rgba[i + 0];   // comp[0] = R
                sumG += rgba[i + 1];   // comp[1] = G
                sumB += rgba[i + 2];   // comp[2] = B
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
// ABGR10 (TrueHDR output) → P010  (BT.2020 / BT.709, 10-bit limited range)
// ────────────────────────────────────────────────────────────────────
// Input:  32-bit ABGR10 packed pixel (A[31:30] B[29:20] G[19:10] R[9:0])
//         (same layout as X2BGR10LE: bits 0-9=R, 10-19=G, 20-29=B, 30-31=unused)
// Output: P010 — uint16_t Y  (w×h),  uint16_t UV interleaved (w×h/2)
//         Data in high bits (<< 6) per FFmpeg P010LE convention.
// Uses floating-point BT.2020 or BT.709 coefficients with strict
// limited-range clamping (Y: 64-940, UV: 64-960) for accurate HDR output.
// Each thread processes one 2×2 block (4 pixels) for chroma sharing.
// ────────────────────────────────────────────────────────────────────
__global__ void abgr10_to_p010_kernel(
    const uint8_t* __restrict__ abgr10, int abgr10_pitch,
    uint8_t* __restrict__ y_plane, int y_pitch,
    uint8_t* __restrict__ uv_plane, int uv_pitch,
    int w, int h, bool bt2020)
{
    int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2; // left pixel of 2×2 block (even)
    int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2; // top row   of 2×2 block (even)
    if (x + 1 >= w || y + 1 >= h) return;

    const uint32_t* src = (const uint32_t*)abgr10;

    // BT.2020 or BT.709 Kr/Kb coefficients
    float Kr = bt2020 ? 0.2627f : 0.2126f;
    float Kb = bt2020 ? 0.0593f : 0.0722f;
    float Kg = 1.0f - Kr - Kb;

    // Accumulate chroma over 2×2 block
    float sumCb = 0.0f, sumCr = 0.0f;
    float Ys[2][2];

    for (int dy = 0; dy < 2; ++dy) {
        const uint32_t* row = src + (y + dy) * (abgr10_pitch / 4);
        for (int dx = 0; dx < 2; ++dx) {
            uint32_t p = row[x + dx];
            float R = (float)(p & 0x3FF)       / 1023.0f;
            float G = (float)((p >> 10) & 0x3FF) / 1023.0f;
            float B = (float)((p >> 20) & 0x3FF) / 1023.0f;

            float Yp = Kr * R + Kg * G + Kb * B;
            Ys[dy][dx] = Yp;
            sumCb += 0.5f * (B - Yp) / (1.0f - Kb);
            sumCr += 0.5f * (R - Yp) / (1.0f - Kr);
        }
    }

    float avgCb = sumCb * 0.25f;
    float avgCr = sumCr * 0.25f;

    // Quantise to 10-bit limited range with proper rounding + high-bit shift
    auto toY  = [](float Y)  -> uint16_t { int v = (int)lrintf(64.0f + Y  * 876.0f); return (uint16_t)(clamp(v, 64, 940) << 6); };
    auto toUV = [](float C)  -> uint16_t { int v = (int)lrintf(512.0f + C * 896.0f); return (uint16_t)(clamp(v, 64, 960) << 6); };

    // Write Y — 4 pixels per 2×2 block
    uint16_t* yRow0 = (uint16_t*)(y_plane + (y + 0) * y_pitch);
    uint16_t* yRow1 = (uint16_t*)(y_plane + (y + 1) * y_pitch);
    yRow0[x + 0] = toY(Ys[0][0]);
    yRow0[x + 1] = toY(Ys[0][1]);
    yRow1[x + 0] = toY(Ys[1][0]);
    yRow1[x + 1] = toY(Ys[1][1]);

    // Write interleaved UV at chroma resolution (w × h/2)
    uint16_t* uvRow = (uint16_t*)(uv_plane + (y / 2) * uv_pitch);
    uvRow[x + 0] = toUV(avgCb);  // U
    uvRow[x + 1] = toUV(avgCr);  // V
}

// ────────────────────────────────────────────────────────────────────
// P010 (10-bit YUV, HDR) → RGBA (8-bit sRGB, SDR)
// HDR to SDR tonemapping for native HDR video input.
// ────────────────────────────────────────────────────────────────────
// Each thread handles one output pixel.
// y_plane:  uint16_t[w × h]    10-bit in high bits (15:6), pitch in bytes
// uv_plane: uint16_t[w × h/2]  interleaved U/V, pitch in bytes
// rgba_out: uint8_t[w × h × 4] RGBA, pitch in bytes
// transfer: 16=PQ(ST.2084), 18=HLG, others=matrix-only (no tonemap)
// ────────────────────────────────────────────────────────────────────

// PQ EOTF (ST.2084): non-linear PQ → linear light (nits)
static __device__ __forceinline__ float pq_eotf(float v) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float vp = powf(fmaxf(v, 0.0f), 1.0f / m2);
    return powf(fmaxf(vp - c1, 0.0f) / fmaxf(c2 - c3 * vp, 1e-6f), 1.0f / m1);
}

// HLG OETF^-1 (linearization): non-linear HLG → linear scene light
static __device__ __forceinline__ float hlg_oetf_inv(float v) {
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;
    if (v <= 0.5f)
        return (v * v) / 3.0f;
    else
        return expf((v - c) / a) + b;
}

static __device__ __forceinline__ float clampf(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

__global__ void p010_to_rgba_sdr_kernel(
    const uint8_t* __restrict__ y_plane, int y_pitch,
    const uint8_t* __restrict__ uv_plane, int uv_pitch,
    uint8_t* __restrict__ rgba_out, int rgba_pitch,
    int w, int h, int transfer)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    // P010: 10-bit data stored in uint16_t high bits (15:6), low bits zero
    const uint16_t* Y16 = (const uint16_t*)(y_plane + y * y_pitch);
    const uint16_t* UV16 = (const uint16_t*)(uv_plane + (y / 2) * uv_pitch);

    int yCode = Y16[x] >> 6;              // 10-bit Y code [0, 1023]
    int uCode = UV16[(x & ~1)] >> 6;      // 10-bit U code
    int vCode = UV16[(x & ~1) + 1] >> 6;  // 10-bit V code

    // Undo limited range: Y [64,940] → [0,1], UV [64,960] → center at 512
    float Ynrm = (float)(yCode - 64) / 876.0f;
    float Unrm = (float)(uCode - 512) / 896.0f;
    float Vnrm = (float)(vCode - 512) / 896.0f;

    // BT.2020 NC YUV → non-linear RGB (same colour space as input transfer)
    float Rnl = Ynrm + 1.4746f * Vnrm;
    float Gnl = Ynrm - 0.1646f * Unrm - 0.5714f * Vnrm;
    float Bnl = Ynrm + 1.8814f * Unrm;

    Rnl = clampf(Rnl, 0.0f, 1.0f);
    Gnl = clampf(Gnl, 0.0f, 1.0f);
    Bnl = clampf(Bnl, 0.0f, 1.0f);

    float Rlin, Glin, Blin;

    if (transfer == 16) {  // PQ (ST.2084)
        // EOTF: non-linear PQ → linear light (nits)
        Rlin = pq_eotf(Rnl);
        Glin = pq_eotf(Gnl);
        Blin = pq_eotf(Bnl);

        // Luminance-based Reinhard tonemapping (SDR target ~100 nits)
        float L = 0.2126f * Rlin + 0.7152f * Glin + 0.0722f * Blin;
        float scale = 1.0f / (1.0f + L / 100.0f);
        Rlin *= scale;
        Glin *= scale;
        Blin *= scale;
    } else if (transfer == 18) {  // HLG
        // OETF^-1: non-linear HLG → linear scene light
        Rlin = hlg_oetf_inv(Rnl);
        Glin = hlg_oetf_inv(Gnl);
        Blin = hlg_oetf_inv(Bnl);

        // HLG nominal peak ~1000 nits, scale to SDR reference
        float L = 0.2126f * Rlin + 0.7152f * Glin + 0.0722f * Blin;
        float scale = 1.0f / (1.0f + L * 10.0f);  // ×10: map HLG peak → SDR
        Rlin *= scale;
        Glin *= scale;
        Blin *= scale;
    } else {
        // Unknown/unspecified: treat linear already, no tonemap
        Rlin = Rnl;
        Glin = Gnl;
        Blin = Bnl;
    }

    // BT.2020 linear → BT.709 linear gamut conversion
    float r709 = 1.6605f * Rlin - 0.5876f * Glin - 0.0728f * Blin;
    float g709 = -0.1246f * Rlin + 1.1329f * Glin - 0.0083f * Blin;
    float b709 = -0.0182f * Rlin - 0.1006f * Glin + 1.1187f * Blin;

    r709 = clampf(r709, 0.0f, 1.0f);
    g709 = clampf(g709, 0.0f, 1.0f);
    b709 = clampf(b709, 0.0f, 1.0f);

    // sRGB gamma encoding (linear → non-linear 8-bit)
    auto srgb_encode = [](float c) -> uint8_t {
        float cs;
        if (c <= 0.0031308f)
            cs = 12.92f * c;
        else
            cs = 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
        return (uint8_t)(clamp((int)(cs * 255.0f + 0.5f), 0, 255));
    };

    int out_idx = y * rgba_pitch + x * 4;
    rgba_out[out_idx + 0] = srgb_encode(r709);             // R (CUDA tex: comp[0])
    rgba_out[out_idx + 1] = srgb_encode(g709);             // G (comp[1])
    rgba_out[out_idx + 2] = srgb_encode(b709);             // B (comp[2])
    rgba_out[out_idx + 3] = 0xFF;                          // A (comp[3])
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

extern "C" void launch_abgr10_to_p010(
    const uint8_t* abgr10, int abgr10_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, bool bt2020, cudaStream_t stream)
{
    dim3 block(16, 16);
    dim3 grid((w + 31) / 32, (h + 31) / 32);
    abgr10_to_p010_kernel<<<grid, block, 0, stream>>>(
        abgr10, abgr10_pitch,
        y_plane, y_pitch, uv_plane, uv_pitch,
        w, h, bt2020);
}

extern "C" void launch_p010_to_rgba_sdr(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, int transfer, cudaStream_t stream)
{
    dim3 block(16, 16);
    dim3 grid((w + 15) / 16, (h + 15) / 16);
    p010_to_rgba_sdr_kernel<<<grid, block, 0, stream>>>(
        y_plane, y_pitch, uv_plane, uv_pitch,
        rgba_out, rgba_pitch, w, h, transfer);
}
