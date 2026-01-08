#pragma once

#include <stdint.h>

#include "../m06_recon/vp8_recon.h"

// Writes a binary PPM (P6) to fd from a YUV420 (I420) image.
// Conversion uses full-range Rec.601 coefficients.
// Returns 0 on success.
int yuv420_write_ppm_fd(int fd, const Yuv420Image* img);
