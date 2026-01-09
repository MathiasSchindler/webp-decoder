#include "quality_psnr.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static double psnr_from_mse(double mse) {
	if (mse <= 0.0) return INFINITY;
	const double maxv = 255.0;
	return 10.0 * log10((maxv * maxv) / mse);
}

int quality_psnr_rgb24(const uint8_t* a_rgb, const uint8_t* b_rgb, uint32_t width,
	                   uint32_t height, QualityPsnr* out) {
	if (!a_rgb || !b_rgb || !out) return -1;
	memset(out, 0, sizeof(*out));
	if (width == 0 || height == 0) return -1;

	uint64_t npx = (uint64_t)width * (uint64_t)height;
	uint64_t nbytes = npx * 3u;

	uint64_t sse_r = 0;
	uint64_t sse_g = 0;
	uint64_t sse_b = 0;
	for (uint64_t i = 0; i < nbytes; i += 3) {
		int dr = (int)a_rgb[i + 0] - (int)b_rgb[i + 0];
		int dg = (int)a_rgb[i + 1] - (int)b_rgb[i + 1];
		int db = (int)a_rgb[i + 2] - (int)b_rgb[i + 2];
		sse_r += (uint64_t)(dr * dr);
		sse_g += (uint64_t)(dg * dg);
		sse_b += (uint64_t)(db * db);
	}

	double mse_r = (double)sse_r / (double)npx;
	double mse_g = (double)sse_g / (double)npx;
	double mse_b = (double)sse_b / (double)npx;
	double mse_rgb = (double)(sse_r + sse_g + sse_b) / (double)(npx * 3u);

	out->npx = npx;
	out->sse_r = sse_r;
	out->sse_g = sse_g;
	out->sse_b = sse_b;
	out->psnr_r = psnr_from_mse(mse_r);
	out->psnr_g = psnr_from_mse(mse_g);
	out->psnr_b = psnr_from_mse(mse_b);
	out->psnr_rgb = psnr_from_mse(mse_rgb);
	return 0;
}