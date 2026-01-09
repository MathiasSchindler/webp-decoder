#pragma once

#include <stdint.h>

typedef struct QualitySsim {
	double ssim_y;
	uint64_t blocks;
} QualitySsim;

/*
Computes SSIM on luma derived from RGB24.

Design choices (must remain stable for regression baselines):
- Luma: full-range integer approx: Y = (77*R + 150*G + 29*B + 128) >> 8
- Windows: non-overlapping blocks starting at (0,0)
- Edge handling: include partial blocks on right/bottom edges
- Aggregation: unweighted average across blocks

Returns 0 on success, -1 on failure.
*/
int quality_ssim_y_from_rgb24(const uint8_t* a_rgb, const uint8_t* b_rgb, uint32_t width,
	                          uint32_t height, QualitySsim* out);