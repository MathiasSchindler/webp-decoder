#pragma once

#include <stdint.h>

typedef struct QualityPsnr {
	double psnr_rgb;
	double psnr_r;
	double psnr_g;
	double psnr_b;
	uint64_t sse_r;
	uint64_t sse_g;
	uint64_t sse_b;
	uint64_t npx;
} QualityPsnr;

/*
Computes PSNR for two RGB24 images.

Inputs:
- a_rgb, b_rgb: byte arrays of length width*height*3

Returns 0 on success, -1 on failure.
*/
int quality_psnr_rgb24(const uint8_t* a_rgb, const uint8_t* b_rgb, uint32_t width,
	                   uint32_t height, QualityPsnr* out);