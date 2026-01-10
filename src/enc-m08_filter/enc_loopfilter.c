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

	// qindex is 0..127. Map it deterministically to VP8 loopfilter params.
	//
	// Goals:
	// - High qindex (low quality): stronger filtering to reduce blocking.
	// - Low qindex (high quality): lighter filtering and higher sharpness to
	//   avoid unnecessary blur.
	//
	// This is a simple piecewise-linear mapping in qindex space.
	int level;
	int sharpness;
	if (qindex < 16) {
		level = ((int)qindex * 10 + 8) / 16; // 0..10
		sharpness = 0;
	} else if (qindex < 32) {
		level = 10 + (((int)qindex - 16) * 10 + 8) / 16; // 10..20
		sharpness = 0;
	} else if (qindex < 64) {
		level = 20 + (((int)qindex - 32) * 16 + 16) / 32; // 20..36
		sharpness = 1;
	} else if (qindex < 96) {
		level = 36 + (((int)qindex - 64) * 16 + 16) / 32; // 36..52
		sharpness = 2;
	} else {
		level = 52 + (((int)qindex - 96) * 11 + 15) / 31; // 52..63
		sharpness = 3;
	}

	*out = (EncVp8LoopFilterParams){
		.use_simple = 0,
		.level = clamp_u8(level, 0, 63),
		.sharpness = clamp_u8(sharpness, 0, 7),
		.use_lf_delta = 0,
	};
}
