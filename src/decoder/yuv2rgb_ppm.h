#pragma once

#include <stdint.h>

#include "vp8_recon.h"

typedef struct {
	uint64_t header_write_ns;
	uint64_t yuv_to_rgb_ns;
	uint64_t pixel_write_ns;
	uint64_t total_ns;
} Yuv420PpmProfile;

// Writes a binary PPM (P6) to fd from a YUV420 (I420) image.
// Conversion uses full-range Rec.601 coefficients.
// Returns 0 on success.
int yuv420_write_ppm_fd(int fd, const Yuv420Image* img);

// Same output as yuv420_write_ppm_fd, with opt-in per-stage timings.
int yuv420_write_ppm_fd_profiled(int fd, const Yuv420Image* img, Yuv420PpmProfile* profile);
