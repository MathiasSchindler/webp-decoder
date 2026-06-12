#include "enc_pad.h"

#include <errno.h>
#include <limits.h>

uint32_t enc_pad16_u32(uint32_t v) {
	if (v > UINT32_MAX - 15u) return 0;
	return (v + 15u) & ~15u;
}

int enc_vp8_mb_grid(uint32_t width, uint32_t height, uint32_t* out_mb_cols, uint32_t* out_mb_rows) {
	if (!out_mb_cols || !out_mb_rows) {
		errno = EINVAL;
		return -1;
	}
	if (width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	// VP8 keyframe stores width/height in 14 bits.
	if (width > 16383u || height > 16383u) {
		errno = EINVAL;
		return -1;
	}

	uint32_t pw = enc_pad16_u32(width);
	uint32_t ph = enc_pad16_u32(height);
	if (pw == 0 || ph == 0) {
		errno = EOVERFLOW;
		return -1;
	}

	*out_mb_cols = pw / 16u;
	*out_mb_rows = ph / 16u;
	if (*out_mb_cols == 0 || *out_mb_rows == 0) {
		errno = EINVAL;
		return -1;
	}
	// Guard against overflow in mb_total in callers.
	if (*out_mb_cols > (1u << 20) || *out_mb_rows > (1u << 20)) {
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}
