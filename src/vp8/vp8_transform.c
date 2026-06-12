#include "vp8_transform.h"

void vp8_inv_wht4x4(const int16_t* input, int16_t* output) {
	int16_t tmp[16];
	for (int i = 0; i < 4; i++) {
		int a1 = input[0 + i] + input[12 + i];
		int b1 = input[4 + i] + input[8 + i];
		int c1 = input[4 + i] - input[8 + i];
		int d1 = input[0 + i] - input[12 + i];

		tmp[0 + i] = (int16_t)(a1 + b1);
		tmp[4 + i] = (int16_t)(c1 + d1);
		tmp[8 + i] = (int16_t)(a1 - b1);
		tmp[12 + i] = (int16_t)(d1 - c1);
	}
	for (int i = 0; i < 4; i++) {
		int a1 = tmp[4 * i + 0] + tmp[4 * i + 3];
		int b1 = tmp[4 * i + 1] + tmp[4 * i + 2];
		int c1 = tmp[4 * i + 1] - tmp[4 * i + 2];
		int d1 = tmp[4 * i + 0] - tmp[4 * i + 3];

		output[4 * i + 0] = (int16_t)((a1 + b1 + 3) >> 3);
		output[4 * i + 1] = (int16_t)((c1 + d1 + 3) >> 3);
		output[4 * i + 2] = (int16_t)((a1 - b1 + 3) >> 3);
		output[4 * i + 3] = (int16_t)((d1 - c1 + 3) >> 3);
	}
}

void vp8_inv_wht4x4_dc_only(int16_t dc, int16_t* output) {
	const int16_t v = (int16_t)(((int)dc + 3) >> 3);
	for (int i = 0; i < 16; i++) output[i] = v;
}

void vp8_inv_dct4x4(const int16_t* input, int16_t* output) {
	static const int cospi8sqrt2minus1 = 20091;
	static const int sinpi8sqrt2 = 35468;

	int16_t tmp[16];
	for (int i = 0; i < 4; i++) {
		int32_t a1 = (int32_t)input[i + 0] + (int32_t)input[i + 8];
		int32_t b1 = (int32_t)input[i + 0] - (int32_t)input[i + 8];

		int32_t temp1 = ((int32_t)input[i + 4] * sinpi8sqrt2) >> 16;
		int32_t temp2 = (int32_t)input[i + 12] + (((int32_t)input[i + 12] * cospi8sqrt2minus1) >> 16);
		int32_t c1 = temp1 - temp2;

		temp1 = (int32_t)input[i + 4] + (((int32_t)input[i + 4] * cospi8sqrt2minus1) >> 16);
		temp2 = ((int32_t)input[i + 12] * sinpi8sqrt2) >> 16;
		int32_t d1 = temp1 + temp2;

		tmp[0 * 4 + i] = (int16_t)(a1 + d1);
		tmp[3 * 4 + i] = (int16_t)(a1 - d1);
		tmp[1 * 4 + i] = (int16_t)(b1 + c1);
		tmp[2 * 4 + i] = (int16_t)(b1 - c1);
	}

	for (int i = 0; i < 4; i++) {
		int32_t a1 = (int32_t)tmp[i * 4 + 0] + (int32_t)tmp[i * 4 + 2];
		int32_t b1 = (int32_t)tmp[i * 4 + 0] - (int32_t)tmp[i * 4 + 2];

		int32_t temp1 = ((int32_t)tmp[i * 4 + 1] * sinpi8sqrt2) >> 16;
		int32_t temp2 = (int32_t)tmp[i * 4 + 3] + (((int32_t)tmp[i * 4 + 3] * cospi8sqrt2minus1) >> 16);
		int32_t c1 = temp1 - temp2;

		temp1 = (int32_t)tmp[i * 4 + 1] + (((int32_t)tmp[i * 4 + 1] * cospi8sqrt2minus1) >> 16);
		temp2 = ((int32_t)tmp[i * 4 + 3] * sinpi8sqrt2) >> 16;
		int32_t d1 = temp1 + temp2;

		output[i * 4 + 0] = (int16_t)((a1 + d1 + 4) >> 3);
		output[i * 4 + 3] = (int16_t)((a1 - d1 + 4) >> 3);
		output[i * 4 + 1] = (int16_t)((b1 + c1 + 4) >> 3);
		output[i * 4 + 2] = (int16_t)((b1 - c1 + 4) >> 3);
	}
}

void vp8_inv_dct4x4_dc_only(int16_t dc, int16_t* output) {
	const int16_t v = (int16_t)(((int)dc + 4) >> 3);
	for (int i = 0; i < 16; i++) output[i] = v;
}
