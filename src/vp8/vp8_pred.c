#include "vp8_pred.h"

#if defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) && \
    !defined(VP8_DISABLE_X86_SIMD)
#include <emmintrin.h>
#define VP8_PRED_SSE2 1
#endif

static inline uint8_t clamp255_i32(int32_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

static inline uint8_t avg3(uint8_t x, uint8_t y, uint8_t z) { return (uint8_t)((x + y + y + z + 2) >> 2); }
static inline uint8_t avg2(uint8_t x, uint8_t y) { return (uint8_t)((x + y + 1) >> 1); }

#if VP8_PRED_SSE2
static inline uint32_t load_u32le(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store_u32le(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
#endif

void vp8_bpred4x4(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint8_t mode) {
#define D(r, c) dst[(uint32_t)(r) * stride + (uint32_t)(c)]

	switch (mode) {
		case 0: {
			uint8_t v = (uint8_t)(((int)A[0] + (int)A[1] + (int)A[2] + (int)A[3] +
			                       (int)L[0] + (int)L[1] + (int)L[2] + (int)L[3] + 4) >>
			                      3);
			D(0, 0) = D(0, 1) = D(0, 2) = D(0, 3) = v;
			D(1, 0) = D(1, 1) = D(1, 2) = D(1, 3) = v;
			D(2, 0) = D(2, 1) = D(2, 2) = D(2, 3) = v;
			D(3, 0) = D(3, 1) = D(3, 2) = D(3, 3) = v;
			break;
		}
		case 1: {
#if VP8_PRED_SSE2
			const __m128i zero = _mm_setzero_si128();
			const __m128i a = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)load_u32le(A)), zero);
			const __m128i p = _mm_set1_epi16((short)A[-1]);
			for (int r = 0; r < 4; r++) {
				const __m128i out = _mm_add_epi16(_mm_sub_epi16(a, p), _mm_set1_epi16((short)L[r]));
				store_u32le(dst + (uint32_t)r * stride, (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(out, zero)));
			}
			break;
#endif
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
			uint8_t e0 = L[3], e1 = L[2], e2 = L[1], e3 = L[0], e4 = A[-1];
			uint8_t e5 = A[0], e6 = A[1], e7 = A[2], e8 = A[3];
			D(3, 0) = avg3(e0, e1, e2);
			D(3, 1) = D(2, 0) = avg3(e1, e2, e3);
			D(3, 2) = D(2, 1) = D(1, 0) = avg3(e2, e3, e4);
			D(3, 3) = D(2, 2) = D(1, 1) = D(0, 0) = avg3(e3, e4, e5);
			D(2, 3) = D(1, 2) = D(0, 1) = avg3(e4, e5, e6);
			D(1, 3) = D(0, 2) = avg3(e5, e6, e7);
			D(0, 3) = avg3(e6, e7, e8);
			break;
		}
		case 6: {
			uint8_t e1 = L[2], e2 = L[1], e3 = L[0], e4 = A[-1];
			uint8_t e5 = A[0], e6 = A[1], e7 = A[2], e8 = A[3];
			uint8_t avg3p_2 = avg3(e1, e2, e3);
			uint8_t avg3p_3 = avg3(e2, e3, e4);
			uint8_t avg3p_4 = avg3(e3, e4, e5);
			uint8_t avg3p_5 = avg3(e4, e5, e6);
			uint8_t avg3p_6 = avg3(e5, e6, e7);
			uint8_t avg3p_7 = avg3(e6, e7, e8);
			uint8_t avg2p_4 = avg2(e4, e5);
			uint8_t avg2p_5 = avg2(e5, e6);
			uint8_t avg2p_6 = avg2(e6, e7);
			uint8_t avg2p_7 = avg2(e7, e8);

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
			uint8_t e0 = L[3], e1 = L[2], e2 = L[1], e3 = L[0], e4 = A[-1];
			uint8_t e5 = A[0], e6 = A[1], e7 = A[2];
			D(3, 0) = avg2(e0, e1);
			D(3, 1) = avg3(e0, e1, e2);
			D(2, 0) = D(3, 2) = avg2(e1, e2);
			D(2, 1) = D(3, 3) = avg3(e1, e2, e3);
			D(2, 2) = D(1, 0) = avg2(e2, e3);
			D(2, 3) = D(1, 1) = avg3(e2, e3, e4);
			D(1, 2) = D(0, 0) = avg2(e3, e4);
			D(1, 3) = D(0, 1) = avg3(e3, e4, e5);
			D(0, 2) = avg3(e4, e5, e6);
			D(0, 3) = avg3(e5, e6, e7);
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
