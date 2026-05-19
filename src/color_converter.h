#pragma once
#include <cuda_runtime.h>
#include <cstdint>

// ────────────────────────────────────────────────────────────────────────
// CUDA colour conversion wrappers  (implemented in cuda_yuv.cu)
// ────────────────────────────────────────────────────────────────────────

extern "C" void launch_nv12_to_rgba(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix, int srcRange);

extern "C" void launch_rgba_to_nv12(
    const uint8_t* rgba, int rgba_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, cudaStream_t stream, int colorMatrix, int srcRange);

extern "C" void launch_abgr10_to_p010(
    const uint8_t* abgr10, int abgr10_pitch,
    uint8_t* y_plane, int y_pitch,
    uint8_t* uv_plane, int uv_pitch,
    int w, int h, bool bt2020, cudaStream_t stream);

extern "C" void launch_p010_to_rgba_sdr(
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    uint8_t* rgba_out, int rgba_pitch,
    int w, int h, int transfer, cudaStream_t stream);

// ────────────────────────────────────────────────────────────────────────
// Shared colour-space utility
//
// Maps AVColorSpace to libswscale SWS_CS_* constant for use with
// sws_setColorspaceDetails / sws_getCoefficients.
// Defined here to eliminate the copy-paste between video_decoder and
// video_encoder.
// ────────────────────────────────────────────────────────────────────────
inline int AvColorSpaceToSWS(int avCS) {
    switch (avCS) {
        case 5:  case 6:  return 0;          // SWS_CS_ITU601
        case 1:           return 1;          // SWS_CS_ITU709
        case 9:  case 10: return 9;          // SWS_CS_BT2020
        default:          return 1;          // default BT.709
    }
}
