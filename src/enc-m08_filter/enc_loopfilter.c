#include "enc_loopfilter.h"

#include <errno.h>

static uint8_t clamp_u8(int v, int lo, int hi) {
	if (v < lo) return (uint8_t)lo;
	if (v > hi) return (uint8_t)hi;
	return (uint8_t)v;
}

void enc_vp8_loopfilter_from_qindex(uint8_t qindex, EncVp8LoopFilterParams* out) {
	if (!out) {
		errno = EINVAL;
		return;
	}

	// qindex is 0..127. Map it to VP8 filter_level 0..63.
	// Use rounded scaling rather than /2 so 127 maps to 63 exactly.
	int level = (int)qindex * 63 + 63;
	level /= 127;

	// Sharpness is a small knob (0..7) that reduces interior filtering.
	// Keep it conservative; increase a bit for very low quality.
	int sharpness = 0;
	if (qindex >= 96) sharpness = 3;
	else if (qindex >= 64) sharpness = 2;
	else if (qindex >= 32) sharpness = 1;

	*out = (EncVp8LoopFilterParams){
		.use_simple = 0,
		.level = clamp_u8(level, 0, 63),
		.sharpness = clamp_u8(sharpness, 0, 7),
		.use_lf_delta = 0,
	};
}
