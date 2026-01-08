#pragma once

#include <stdint.h>

#include "../m06_recon/vp8_recon.h"

// Writes an RGB PNG (IHDR color_type=2, bit_depth=8) to fd from a YUV420 (I420) image.
// Encoding uses filter type 0 for every scanline and zlib/DEFLATE with stored (uncompressed) blocks.
// Returns 0 on success.
int yuv420_write_png_fd(int fd, const Yuv420Image* img);
