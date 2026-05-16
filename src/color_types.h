#pragma once

// Simplified color matrix identifiers for YUV↔RGB conversion
enum ColorMatrix : int {
    COLOR_MATRIX_BT601  = 0,
    COLOR_MATRIX_BT709  = 1,  // default for HD content
    COLOR_MATRIX_BT2020 = 2,  // BT.2020 NCL
};

// Pixel range for luma/chroma
enum ColorSrcRange : int {
    COLOR_RANGE_UNSPECIFIED = 0,
    COLOR_RANGE_LIMITED     = 1,  // Y: 16-235, Cb/Cr: 16-240 (MPEG)
    COLOR_RANGE_FULL        = 2,  // Y: 0-255, Cb/Cr: 1-255 (JPEG)
};
