#include "quality_ssim.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static inline uint8_t luma_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
	/* Full-range, deterministic integer approximation. */
	return (uint8_t)((77u * (unsigned)r + 150u * (unsigned)g + 29u * (unsigned)b + 128u) >> 8);
}

int quality_ssim_y_from_rgb24(const uint8_t* a_rgb, const uint8_t* b_rgb, uint32_t width,
	                          uint32_t height, QualitySsim* out) {
	if (!a_rgb || !b_rgb || !out) return -1;
	memset(out, 0, sizeof(*out));
	if (width == 0 || height == 0) return -1;

	/* Standard constants (single-scale SSIM). */
	const double L = 255.0;
	const double K1 = 0.01;
	const double K2 = 0.03;
	const double C1 = (K1 * L) * (K1 * L);
	const double C2 = (K2 * L) * (K2 * L);

	const uint32_t block = 8;
	uint64_t blocks = 0;
	double sum_ssim = 0.0;

	for (uint32_t y0 = 0; y0 < height; y0 += block) {
		uint32_t bh = height - y0;
		if (bh > block) bh = block;
		for (uint32_t x0 = 0; x0 < width; x0 += block) {
			uint32_t bw = width - x0;
			if (bw > block) bw = block;

			uint64_t n = (uint64_t)bw * (uint64_t)bh;
			if (n == 0) continue;

			uint64_t sum_x = 0, sum_y = 0;
			uint64_t sum_x2 = 0, sum_y2 = 0;
			uint64_t sum_xy = 0;

			for (uint32_t dy = 0; dy < bh; dy++) {
				uint32_t y = y0 + dy;
				uint64_t row_off = (uint64_t)y * (uint64_t)width * 3u;
				for (uint32_t dx = 0; dx < bw; dx++) {
					uint32_t x = x0 + dx;
					uint64_t i = row_off + (uint64_t)x * 3u;
					uint8_t ar = a_rgb[i + 0];
					uint8_t ag = a_rgb[i + 1];
					uint8_t ab = a_rgb[i + 2];
					uint8_t br = b_rgb[i + 0];
					uint8_t bg = b_rgb[i + 1];
					uint8_t bb = b_rgb[i + 2];

					uint32_t xY = (uint32_t)luma_from_rgb(ar, ag, ab);
					uint32_t yY = (uint32_t)luma_from_rgb(br, bg, bb);

					sum_x += xY;
					sum_y += yY;
					sum_x2 += (uint64_t)(xY * xY);
					sum_y2 += (uint64_t)(yY * yY);
					sum_xy += (uint64_t)(xY * yY);
				}
			}

			double inv_n = 1.0 / (double)n;
			double mu_x = (double)sum_x * inv_n;
			double mu_y = (double)sum_y * inv_n;

			double ex2 = (double)sum_x2 * inv_n;
			double ey2 = (double)sum_y2 * inv_n;
			double exy = (double)sum_xy * inv_n;

			double var_x = ex2 - mu_x * mu_x;
			double var_y = ey2 - mu_y * mu_y;
			double cov_xy = exy - mu_x * mu_y;

			/* Numerical safety: variance can be tiny negative from rounding. */
			if (var_x < 0.0 && var_x > -1e-12) var_x = 0.0;
			if (var_y < 0.0 && var_y > -1e-12) var_y = 0.0;

			double num = (2.0 * mu_x * mu_y + C1) * (2.0 * cov_xy + C2);
			double den = (mu_x * mu_x + mu_y * mu_y + C1) * (var_x + var_y + C2);

			double ssim = 0.0;
			if (den != 0.0) {
				ssim = num / den;
			}

			sum_ssim += ssim;
			blocks++;
		}
	}

	if (blocks == 0) return -1;
	out->blocks = blocks;
	out->ssim_y = sum_ssim / (double)blocks;
	return 0;
}