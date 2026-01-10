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
	// Keep this mapping aligned with libwebp defaults for apples-to-apples
	// comparisons. Empirically, cwebp uses sharpness=0 and a relatively low
	// filter level for most qindex values.
	//
	// We approximate cwebp's behavior with a small piecewise-linear curve anchored
	// on observed points (qindex -> level):
	//   0->0, 9->3, 26->8, 38->11, 75->29, 127->63
	int level;
	const int sharpness = 0;
	if (qindex <= 26) {
		level = ((int)qindex * 8 + 13) / 26; // round(qindex * 8/26)
	} else if (qindex <= 38) {
		level = 8 + (((int)qindex - 26) * 3 + 6) / 12; // round((q-26) * 3/12)
	} else if (qindex <= 75) {
		level = 11 + (((int)qindex - 38) * 18 + 18) / 37; // round((q-38) * 18/37)
	} else {
		level = 29 + (((int)qindex - 75) * 34 + 26) / 52; // round((q-75) * 34/52)
	}

	*out = (EncVp8LoopFilterParams){
		.use_simple = 0,
		.level = clamp_u8(level, 0, 63),
		.sharpness = clamp_u8(sharpness, 0, 7),
		.use_lf_delta = 0,
	};
}
