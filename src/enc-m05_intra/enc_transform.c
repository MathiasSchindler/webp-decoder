#include "enc_transform.h"

#include <stddef.h>

void enc_vp8_ftransform4x4(const uint8_t* src,
                           int src_stride,
                           const uint8_t* ref,
                           int ref_stride,
                           int16_t out[16]) {
	int tmp[16];
	for (int i = 0; i < 4; ++i) {
		const uint8_t* s = src + (ptrdiff_t)i * src_stride;
		const uint8_t* r = ref + (ptrdiff_t)i * ref_stride;

		const int d0 = (int)s[0] - (int)r[0];
		const int d1 = (int)s[1] - (int)r[1];
		const int d2 = (int)s[2] - (int)r[2];
		const int d3 = (int)s[3] - (int)r[3];

		const int a0 = d0 + d3;
		const int a1 = d1 + d2;
		const int a2 = d1 - d2;
		const int a3 = d0 - d3;

		tmp[0 + i * 4] = (a0 + a1) * 8;
		tmp[1 + i * 4] = (a2 * 2217 + a3 * 5352 + 1812) >> 9;
		tmp[2 + i * 4] = (a0 - a1) * 8;
		tmp[3 + i * 4] = (a3 * 2217 - a2 * 5352 + 937) >> 9;
	}

	for (int i = 0; i < 4; ++i) {
		const int a0 = tmp[0 + i] + tmp[12 + i];
		const int a1 = tmp[4 + i] + tmp[8 + i];
		const int a2 = tmp[4 + i] - tmp[8 + i];
		const int a3 = tmp[0 + i] - tmp[12 + i];

		out[0 + i] = (int16_t)((a0 + a1 + 7) >> 4);
		out[4 + i] = (int16_t)(((a2 * 2217 + a3 * 5352 + 12000) >> 16) + (a3 != 0));
		out[8 + i] = (int16_t)((a0 - a1 + 7) >> 4);
		out[12 + i] = (int16_t)((a3 * 2217 - a2 * 5352 + 51000) >> 16);
	}
}

void enc_vp8_ftransform_wht(const int16_t* in, int16_t out[16]) {
	int32_t tmp[16];
	for (int i = 0; i < 4; ++i, in += 64) {
		const int a0 = (int)in[0 * 16] + (int)in[2 * 16];
		const int a1 = (int)in[1 * 16] + (int)in[3 * 16];
		const int a2 = (int)in[1 * 16] - (int)in[3 * 16];
		const int a3 = (int)in[0 * 16] - (int)in[2 * 16];
		tmp[0 + i * 4] = a0 + a1;
		tmp[1 + i * 4] = a3 + a2;
		tmp[2 + i * 4] = a3 - a2;
		tmp[3 + i * 4] = a0 - a1;
	}
	for (int i = 0; i < 4; ++i) {
		const int a0 = (int)(tmp[0 + i] + tmp[8 + i]);
		const int a1 = (int)(tmp[4 + i] + tmp[12 + i]);
		const int a2 = (int)(tmp[4 + i] - tmp[12 + i]);
		const int a3 = (int)(tmp[0 + i] - tmp[8 + i]);

		const int b0 = a0 + a1;
		const int b1 = a3 + a2;
		const int b2 = a3 - a2;
		const int b3 = a0 - a1;

		out[0 + i] = (int16_t)(b0 >> 1);
		out[4 + i] = (int16_t)(b1 >> 1);
		out[8 + i] = (int16_t)(b2 >> 1);
		out[12 + i] = (int16_t)(b3 >> 1);
	}
}
