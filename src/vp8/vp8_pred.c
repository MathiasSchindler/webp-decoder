#include "vp8_pred.h"

static inline uint8_t clamp255_i32(int32_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

static inline uint8_t avg3(uint8_t x, uint8_t y, uint8_t z) { return (uint8_t)((x + y + y + z + 2) >> 2); }
static inline uint8_t avg2(uint8_t x, uint8_t y) { return (uint8_t)((x + y + 1) >> 1); }

void vp8_bpred4x4(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint8_t mode) {
#define D(r, c) dst[(uint32_t)(r) * stride + (uint32_t)(c)]
	uint8_t E[9];
	E[0] = L[3];
	E[1] = L[2];
	E[2] = L[1];
	E[3] = L[0];
	E[4] = A[-1];
	E[5] = A[0];
	E[6] = A[1];
	E[7] = A[2];
	E[8] = A[3];

	switch (mode) {
		case 0: {
			int v = 4;
			for (int i = 0; i < 4; i++) v += (int)A[i] + (int)L[i];
			v >>= 3;
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) D(r, c) = (uint8_t)v;
			break;
		}
		case 1: {
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) D(r, c) = clamp255_i32((int32_t)L[r] + (int32_t)A[c] - (int32_t)A[-1]);
			break;
		}
		case 2: {
			for (int c = 0; c < 4; c++) {
				uint8_t v = avg3(A[c - 1], A[c], A[c + 1]);
				D(0, c) = D(1, c) = D(2, c) = D(3, c) = v;
			}
			break;
		}
		case 3: {
			uint8_t v = avg3(L[2], L[3], L[3]);
			D(3, 0) = D(3, 1) = D(3, 2) = D(3, 3) = v;
			v = avg3(L[1], L[2], L[3]);
			D(2, 0) = D(2, 1) = D(2, 2) = D(2, 3) = v;
			v = avg3(L[0], L[1], L[2]);
			D(1, 0) = D(1, 1) = D(1, 2) = D(1, 3) = v;
			v = avg3(A[-1], L[0], L[1]);
			D(0, 0) = D(0, 1) = D(0, 2) = D(0, 3) = v;
			break;
		}
		case 4: {
			D(0, 0) = avg3(A[0], A[1], A[2]);
			D(0, 1) = D(1, 0) = avg3(A[1], A[2], A[3]);
			D(0, 2) = D(1, 1) = D(2, 0) = avg3(A[2], A[3], A[4]);
			D(0, 3) = D(1, 2) = D(2, 1) = D(3, 0) = avg3(A[3], A[4], A[5]);
			D(1, 3) = D(2, 2) = D(3, 1) = avg3(A[4], A[5], A[6]);
			D(2, 3) = D(3, 2) = avg3(A[5], A[6], A[7]);
			D(3, 3) = avg3(A[6], A[7], A[7]);
			break;
		}
		case 5: {
			D(3, 0) = avg3(E[0], E[1], E[2]);
			D(3, 1) = D(2, 0) = avg3(E[1], E[2], E[3]);
			D(3, 2) = D(2, 1) = D(1, 0) = avg3(E[2], E[3], E[4]);
			D(3, 3) = D(2, 2) = D(1, 1) = D(0, 0) = avg3(E[3], E[4], E[5]);
			D(2, 3) = D(1, 2) = D(0, 1) = avg3(E[4], E[5], E[6]);
			D(1, 3) = D(0, 2) = avg3(E[5], E[6], E[7]);
			D(0, 3) = avg3(E[6], E[7], E[8]);
			break;
		}
		case 6: {
			uint8_t avg3p_2 = avg3(E[1], E[2], E[3]);
			uint8_t avg3p_3 = avg3(E[2], E[3], E[4]);
			uint8_t avg3p_4 = avg3(E[3], E[4], E[5]);
			uint8_t avg3p_5 = avg3(E[4], E[5], E[6]);
			uint8_t avg3p_6 = avg3(E[5], E[6], E[7]);
			uint8_t avg3p_7 = avg3(E[6], E[7], E[8]);
			uint8_t avg2p_4 = avg2(E[4], E[5]);
			uint8_t avg2p_5 = avg2(E[5], E[6]);
			uint8_t avg2p_6 = avg2(E[6], E[7]);
			uint8_t avg2p_7 = avg2(E[7], E[8]);

			D(3, 0) = avg3p_2;
			D(2, 0) = avg3p_3;
			D(3, 1) = D(1, 0) = avg3p_4;
			D(2, 1) = D(0, 0) = avg2p_4;
			D(3, 2) = D(1, 1) = avg3p_5;
			D(2, 2) = D(0, 1) = avg2p_5;
			D(3, 3) = D(1, 2) = avg3p_6;
			D(2, 3) = D(0, 2) = avg2p_6;
			D(1, 3) = avg3p_7;
			D(0, 3) = avg2p_7;
			break;
		}
		case 7: {
			D(0, 0) = avg2(A[0], A[1]);
			D(1, 0) = avg3(A[0], A[1], A[2]);
			D(2, 0) = D(0, 1) = avg2(A[1], A[2]);
			D(1, 1) = D(3, 0) = avg3(A[1], A[2], A[3]);
			D(2, 1) = D(0, 2) = avg2(A[2], A[3]);
			D(3, 1) = D(1, 2) = avg3(A[2], A[3], A[4]);
			D(2, 2) = D(0, 3) = avg2(A[3], A[4]);
			D(3, 2) = D(1, 3) = avg3(A[3], A[4], A[5]);
			D(2, 3) = avg3(A[4], A[5], A[6]);
			D(3, 3) = avg3(A[5], A[6], A[7]);
			break;
		}
		case 8: {
			D(3, 0) = avg2(E[0], E[1]);
			D(3, 1) = avg3(E[0], E[1], E[2]);
			D(2, 0) = D(3, 2) = avg2(E[1], E[2]);
			D(2, 1) = D(3, 3) = avg3(E[1], E[2], E[3]);
			D(2, 2) = D(1, 0) = avg2(E[2], E[3]);
			D(2, 3) = D(1, 1) = avg3(E[2], E[3], E[4]);
			D(1, 2) = D(0, 0) = avg2(E[3], E[4]);
			D(1, 3) = D(0, 1) = avg3(E[3], E[4], E[5]);
			D(0, 2) = avg3(E[4], E[5], E[6]);
			D(0, 3) = avg3(E[5], E[6], E[7]);
			break;
		}
		case 9: {
			D(0, 0) = avg2(L[0], L[1]);
			D(0, 1) = avg3(L[0], L[1], L[2]);
			D(0, 2) = D(1, 0) = avg2(L[1], L[2]);
			D(0, 3) = D(1, 1) = avg3(L[1], L[2], L[3]);
			D(1, 2) = D(2, 0) = avg2(L[2], L[3]);
			D(1, 3) = D(2, 1) = avg3(L[2], L[3], L[3]);
			for (int r = 2; r < 4; r++) {
				for (int c = 2; c < 4; c++) D(r, c) = L[3];
			}
			D(3, 0) = L[3];
			D(3, 1) = L[3];
			break;
		}
		default: {
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) D(r, c) = 128;
			break;
		}
	}
#undef D
}
