#include "vp8_recon.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "vp8_loopfilter.h"
#include "../vp8/vp8_pred.h"
#include "../vp8/vp8_quant.h"
#include "../vp8/vp8_transform.h"

// --- Helpers ---

static inline uint8_t clamp255_i32(int32_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

static inline int dc_q(int q) { return vp8_dc_q(q); }
static inline int ac_q(int q) { return vp8_ac_q(q); }

typedef enum {
	TOKEN_BLOCK_Y1 = 0,
	TOKEN_BLOCK_UV = 1,
	TOKEN_BLOCK_Y2 = 2,
} TokenBlock;

typedef struct {
	int quant_idx;
	int factor[3][2];
} DequantFactors;

static void dequant_init(DequantFactors* dqf, const Vp8DecodedFrame* decoded) {
	// Mirrors RFC 6386 reference dequant_init().
	int seg_count = decoded->segmentation_enabled ? 4 : 1;
	for (int i = 0; i < seg_count; i++) {
		int q = (int)decoded->q_index;
		if (decoded->segmentation_enabled) {
			q = decoded->segmentation_abs ? decoded->seg_quant_idx[i] : (q + decoded->seg_quant_idx[i]);
		}
		dqf[i].quant_idx = q;

		dqf[i].factor[TOKEN_BLOCK_Y1][0] = dc_q(q + decoded->y1_dc_delta_q);
		dqf[i].factor[TOKEN_BLOCK_Y1][1] = ac_q(q);
		dqf[i].factor[TOKEN_BLOCK_UV][0] = dc_q(q + decoded->uv_dc_delta_q);
		dqf[i].factor[TOKEN_BLOCK_UV][1] = ac_q(q + decoded->uv_ac_delta_q);
		dqf[i].factor[TOKEN_BLOCK_Y2][0] = dc_q(q + decoded->y2_dc_delta_q) * 2;
		dqf[i].factor[TOKEN_BLOCK_Y2][1] = ac_q(q + decoded->y2_ac_delta_q) * 155 / 100;
		if (dqf[i].factor[TOKEN_BLOCK_Y2][1] < 8) dqf[i].factor[TOKEN_BLOCK_Y2][1] = 8;
		if (dqf[i].factor[TOKEN_BLOCK_UV][0] > 132) dqf[i].factor[TOKEN_BLOCK_UV][0] = 132;
	}
}

// --- Inverse transforms from RFC 6386 ---

static void inv_wht4x4(const int16_t* in, int16_t* out) { vp8_inv_wht4x4(in, out); }

static void inv_dct4x4(const int16_t* input, int16_t* output) { vp8_inv_dct4x4(input, output); }

// --- Prediction ---

static void pred_dc(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint32_t n, int have_above,
                    int have_left, uint8_t above_oob, uint8_t left_oob) {
	if (!have_above && !have_left) {
		for (uint32_t r = 0; r < n; r++)
			for (uint32_t c = 0; c < n; c++) dst[r * stride + c] = 128;
		return;
	}
	int sum = 0;
	int shf = 0;
	if (have_above && have_left) {
		for (uint32_t i = 0; i < n; i++) sum += (int)A[i] + (int)L[i];
		shf = (n == 16) ? 5 : (n == 8) ? 4 : 3;
	} else if (have_left) {
		for (uint32_t i = 0; i < n; i++) sum += (int)L[i];
		shf = (n == 16) ? 4 : (n == 8) ? 3 : 2;
	} else {
		for (uint32_t i = 0; i < n; i++) sum += (int)A[i];
		shf = (n == 16) ? 4 : (n == 8) ? 3 : 2;
	}
	uint8_t v = (uint8_t)((sum + (1 << (shf - 1))) >> shf);
	(void)above_oob;
	(void)left_oob;
	for (uint32_t r = 0; r < n; r++)
		for (uint32_t c = 0; c < n; c++) dst[r * stride + c] = v;
}

static void pred_v(uint8_t* dst, uint32_t stride, const uint8_t* A, uint32_t n, int have_above, uint8_t above_oob) {
	for (uint32_t r = 0; r < n; r++) {
		for (uint32_t c = 0; c < n; c++) dst[r * stride + c] = have_above ? A[c] : above_oob;
	}
}

static void pred_h(uint8_t* dst, uint32_t stride, const uint8_t* L, uint32_t n, int have_left, uint8_t left_oob) {
	for (uint32_t r = 0; r < n; r++) {
		uint8_t v = have_left ? L[r] : left_oob;
		for (uint32_t c = 0; c < n; c++) dst[r * stride + c] = v;
	}
}

static void pred_tm(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint32_t n, int have_above,
                    int have_left, uint8_t above_oob, uint8_t left_oob) {
	uint8_t P = 128;
	if (have_above && have_left) {
		P = A[-1];
	} else if (!have_above && have_left) {
		P = above_oob; // A[-1]
	} else if (have_above && !have_left) {
		P = left_oob; // L[-1]
	} else {
		// For TM_PRED, use the out-of-bounds value for the pixel above-left.
		// RFC 6386 Section 12: pixels above the top row (including above-left) are 127.
		P = above_oob;
	}
	for (uint32_t r = 0; r < n; r++) {
		uint8_t Lv = have_left ? L[r] : left_oob;
		for (uint32_t c = 0; c < n; c++) {
			uint8_t Av = have_above ? A[c] : above_oob;
			dst[r * stride + c] = clamp255_i32((int32_t)Lv + (int32_t)Av - (int32_t)P);
		}
	}
}

static void subblock_predict(uint8_t B[4][4], const uint8_t* A, const uint8_t* L, uint8_t mode) {
	vp8_bpred4x4(&B[0][0], 4, A, L, mode);
}

int yuv420_alloc(Yuv420Image* img, uint32_t width, uint32_t height) {
	if (!img || width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	*img = (Yuv420Image){0};
	img->width = width;
	img->height = height;
	img->stride_y = width;
	img->stride_uv = (width + 1u) / 2u;
	size_t ysz = (size_t)img->stride_y * (size_t)height;
	size_t uvh = (size_t)((height + 1u) / 2u);
	size_t uvsz = (size_t)img->stride_uv * uvh;
	img->y = (uint8_t*)malloc(ysz);
	img->u = (uint8_t*)malloc(uvsz);
	img->v = (uint8_t*)malloc(uvsz);
	if (!img->y || !img->u || !img->v) {
		yuv420_free(img);
		errno = ENOMEM;
		return -1;
	}
	memset(img->y, 0, ysz);
	memset(img->u, 128, uvsz);
	memset(img->v, 128, uvsz);
	return 0;
}

void yuv420_free(Yuv420Image* img) {
	if (!img) return;
	free(img->y);
	free(img->u);
	free(img->v);
	*img = (Yuv420Image){0};
}

static void get_above_row(const uint8_t* plane, uint32_t stride, uint32_t width, uint32_t x, uint32_t y, uint32_t n,
                          uint8_t fill, uint8_t* out) {
	if (y == 0) {
		for (uint32_t i = 0; i < n; i++) out[i] = fill;
		return;
	}
	uint32_t row = y - 1;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t xx = x + i;
		if (xx >= width) xx = width - 1;
		out[i] = plane[row * stride + xx];
	}
}

static void get_left_col(const uint8_t* plane, uint32_t stride, uint32_t height, uint32_t x, uint32_t y, uint32_t n,
                         uint8_t fill, uint8_t* out) {
	if (x == 0) {
		for (uint32_t i = 0; i < n; i++) out[i] = fill;
		return;
	}
	uint32_t col = x - 1;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t yy = y + i;
		if (yy >= height) yy = height - 1;
		out[i] = plane[yy * stride + col];
	}
}

static int vp8_reconstruct_keyframe_yuv_internal(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out,
								  int apply_loopfilter) {
	if (!kf || !decoded || !out) {
		errno = EINVAL;
		return -1;
	}

	// Reconstruct into a macroblock-aligned padded buffer first.
	// This matches reference decoders that reconstruct full macroblocks even when the
	// visible frame dimensions are not multiples of 16 (or chroma not multiples of 8).
	uint32_t padded_w = decoded->mb_cols * 16u;
	uint32_t padded_h = decoded->mb_rows * 16u;
	Yuv420Image pad;
	if (yuv420_alloc(&pad, padded_w, padded_h) != 0) return -1;

	DequantFactors dqf[4];
	memset(dqf, 0, sizeof(dqf));
	dequant_init(dqf, decoded);

	uint32_t mb_cols = decoded->mb_cols;
	uint32_t mb_rows = decoded->mb_rows;
	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb = mb_r * mb_cols + mb_c;
			uint32_t seg = decoded->segmentation_enabled ? (uint32_t)(decoded->segment_id[mb] & 3u) : 0u;
			const DequantFactors* q = &dqf[seg];

			uint32_t x = mb_c * 16u;
			uint32_t y = mb_r * 16u;

			uint8_t ymode = decoded->ymode[mb];
			if (ymode == 4) {
				// B_PRED (4x4 intra): each subblock predictor depends on already-constructed pixels,
				// including those inside the current macroblock. Reconstruct in scan order.
				for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
					for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
						uint32_t sb = sb_r * 4u + sb_c;
						uint8_t mode = decoded->bmode[mb * 16u + sb];
						uint32_t sx = x + sb_c * 4u;
						uint32_t sy = y + sb_r * 4u;

						uint8_t A8[9];
						uint8_t L4[4];
						// Top-left (P) value.
						if (sy == 0) A8[0] = 127;
						else if (sx == 0) A8[0] = 129;
						else A8[0] = pad.y[(sy - 1) * pad.stride_y + (sx - 1)];

						// Above row (A[0..7] lives in A8[1..8]).
						for (uint32_t i = 0; i < 8; i++) {
							if (sy == 0) {
								A8[1 + i] = 127;
								continue;
							}
							uint32_t row = sy - 1;
							uint32_t col;
							if (sb_c == 3 && i >= 4) {
								// Right-edge special case: use pixels above macroblock x+16..19 (RFC 6386 11.4).
								if (y == 0) {
									A8[1 + i] = 127;
									continue;
								}
								row = y - 1;
								col = x + 16u + (i - 4u);
							} else {
								col = sx + i;
							}
							if (row >= pad.height) row = pad.height - 1;
							if (col >= pad.width) col = pad.width - 1;
							A8[1 + i] = pad.y[row * pad.stride_y + col];
						}

						// Left column.
						if (sx == 0) {
							for (uint32_t i = 0; i < 4; i++) L4[i] = 129;
						} else {
							for (uint32_t i = 0; i < 4; i++) {
								uint32_t row = sy + i;
								if (row >= pad.height) row = pad.height - 1;
								L4[i] = pad.y[row * pad.stride_y + (sx - 1)];
							}
						}

						uint8_t B[4][4];
						subblock_predict(B, &A8[1], L4, mode);

						uint32_t blk = mb * 16u + (sb_r * 4u + sb_c);
						const int16_t* cq = decoded->coeff_y + (size_t)blk * 16u;
						int16_t cdeq[16];
						for (int i = 0; i < 16; i++) {
							int fct = (i == 0) ? q->factor[TOKEN_BLOCK_Y1][0] : q->factor[TOKEN_BLOCK_Y1][1];
							cdeq[i] = (int16_t)(cq[i] * fct);
						}
						int16_t res[16];
						inv_dct4x4(cdeq, res);

						for (uint32_t rr = 0; rr < 4; rr++) {
							uint32_t yy = sy + rr;
							if (yy >= pad.height) continue;
							for (uint32_t cc = 0; cc < 4; cc++) {
								uint32_t xx = sx + cc;
								if (xx >= pad.width) continue;
								pad.y[yy * pad.stride_y + xx] =
								    clamp255_i32((int32_t)B[rr][cc] + (int32_t)res[(int)rr * 4 + (int)cc]);
							}
						}
					}
				}
			} else {

				// Build luma predictor into a temporary 16x16 block.
				uint8_t pred_y[16 * 16];
				uint8_t A16[20];
				uint8_t L16[16];
				get_above_row(pad.y, pad.stride_y, pad.width, x, y, 16, 127, A16);
				get_left_col(pad.y, pad.stride_y, pad.height, x, y, 16, 129, L16);
				A16[16] = A16[15];
				A16[17] = A16[15];
				A16[18] = A16[15];
				A16[19] = A16[15];
				int have_above = (y != 0);
				int have_left = (x != 0);

				// 16x16 predictors.
				switch (ymode) {
					case 0: pred_dc(pred_y, 16, A16, L16, 16, have_above, have_left, 127, 129); break;
					case 1: pred_v(pred_y, 16, A16, 16, have_above, 127); break;
					case 2: pred_h(pred_y, 16, L16, 16, have_left, 129); break;
					case 3: {
						// Need A[-1] for TM; model it as 127/129 for OOB.
						uint8_t Ap[17];
						Ap[0] = have_above && have_left ? pad.y[(y - 1) * pad.stride_y + (x - 1)] : (have_above ? 129 : 127);
						memcpy(&Ap[1], A16, 16);
						pred_tm(pred_y, 16, &Ap[1], L16, 16, have_above, have_left, 127, 129);
						break;
					}
					default: pred_dc(pred_y, 16, A16, L16, 16, have_above, have_left, 127, 129); break;
				}

				// Inverse transforms and add residue for luma.
				int16_t y2_dc[16];
				memset(y2_dc, 0, sizeof(y2_dc));
				int16_t y2_deq[16];
				const int16_t* y2q = decoded->coeff_y2 + (size_t)mb * 16u;
				for (int i = 0; i < 16; i++) {
					int fct = (i == 0) ? q->factor[TOKEN_BLOCK_Y2][0] : q->factor[TOKEN_BLOCK_Y2][1];
					y2_deq[i] = (int16_t)(y2q[i] * fct);
				}
				inv_wht4x4(y2_deq, y2_dc);

				for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
					for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
						uint32_t blk = mb * 16u + (sb_r * 4u + sb_c);
						const int16_t* cq = decoded->coeff_y + (size_t)blk * 16u;
						int16_t cdeq[16];
						for (int i = 0; i < 16; i++) {
							if (i == 0) {
								// With Y2 present, the per-block DC comes from inverse WHT of already-dequantized Y2.
								cdeq[i] = y2_dc[(int)sb_r * 4 + (int)sb_c];
							} else {
								int fct = q->factor[TOKEN_BLOCK_Y1][1];
								cdeq[i] = (int16_t)(cq[i] * fct);
							}
						}
						int16_t res[16];
						inv_dct4x4(cdeq, res);

						for (uint32_t rr = 0; rr < 4; rr++) {
							uint32_t yy = y + sb_r * 4u + rr;
							if (yy >= pad.height) continue;
							for (uint32_t cc = 0; cc < 4; cc++) {
								uint32_t xx = x + sb_c * 4u + cc;
								if (xx >= pad.width) continue;
								uint8_t p = pred_y[(sb_r * 4u + rr) * 16u + (sb_c * 4u + cc)];
								pad.y[yy * pad.stride_y + xx] =
								    clamp255_i32((int32_t)p + (int32_t)res[(int)rr * 4 + (int)cc]);
							}
						}
					}
				}
			}

			// Chroma predictors (8x8) and inverse transforms.
			uint32_t cx = mb_c * 8u;
			uint32_t cy = mb_r * 8u;
			uint32_t cw = (pad.width + 1u) / 2u;
			uint32_t ch = (pad.height + 1u) / 2u;

			uint8_t pred_u[8 * 8];
			uint8_t pred_vp[8 * 8];
			uint8_t A8u[8];
			uint8_t L8u[8];
			uint8_t A8v[8];
			uint8_t L8v[8];
			get_above_row(pad.u, pad.stride_uv, cw, cx, cy, 8, 127, A8u);
			get_left_col(pad.u, pad.stride_uv, ch, cx, cy, 8, 129, L8u);
			get_above_row(pad.v, pad.stride_uv, cw, cx, cy, 8, 127, A8v);
			get_left_col(pad.v, pad.stride_uv, ch, cx, cy, 8, 129, L8v);
			int have_above_c = (cy != 0);
			int have_left_c = (cx != 0);
			switch (decoded->uv_mode[mb]) {
				case 0:
					pred_dc(pred_u, 8, A8u, L8u, 8, have_above_c, have_left_c, 127, 129);
					pred_dc(pred_vp, 8, A8v, L8v, 8, have_above_c, have_left_c, 127, 129);
					break;
				case 1:
					pred_v(pred_u, 8, A8u, 8, have_above_c, 127);
					pred_v(pred_vp, 8, A8v, 8, have_above_c, 127);
					break;
				case 2:
					pred_h(pred_u, 8, L8u, 8, have_left_c, 129);
					pred_h(pred_vp, 8, L8v, 8, have_left_c, 129);
					break;
				case 3: {
					uint8_t Apu[9];
					uint8_t Apv[9];
					Apu[0] = have_above_c && have_left_c ? pad.u[(cy - 1) * pad.stride_uv + (cx - 1)] : (have_above_c ? 129 : 127);
					Apv[0] = have_above_c && have_left_c ? pad.v[(cy - 1) * pad.stride_uv + (cx - 1)] : (have_above_c ? 129 : 127);
					memcpy(&Apu[1], A8u, 8);
					memcpy(&Apv[1], A8v, 8);
					pred_tm(pred_u, 8, &Apu[1], L8u, 8, have_above_c, have_left_c, 127, 129);
					pred_tm(pred_vp, 8, &Apv[1], L8v, 8, have_above_c, have_left_c, 127, 129);
					break;
				}
				default:
					pred_dc(pred_u, 8, A8u, L8u, 8, have_above_c, have_left_c, 127, 129);
					pred_dc(pred_vp, 8, A8v, L8v, 8, have_above_c, have_left_c, 127, 129);
					break;
			}

			for (uint32_t b = 0; b < 4; b++) {
				uint32_t br = b / 2u;
				uint32_t bc = b % 2u;
				const int16_t* cuq = decoded->coeff_u + ((size_t)mb * 4u + b) * 16u;
				const int16_t* cvq = decoded->coeff_v + ((size_t)mb * 4u + b) * 16u;
				int16_t cudeq[16];
				int16_t cvdeq[16];
				for (int i = 0; i < 16; i++) {
					int fct = (i == 0) ? q->factor[TOKEN_BLOCK_UV][0] : q->factor[TOKEN_BLOCK_UV][1];
					cudeq[i] = (int16_t)(cuq[i] * fct);
					cvdeq[i] = (int16_t)(cvq[i] * fct);
				}
				int16_t ures[16];
				int16_t vres[16];
				inv_dct4x4(cudeq, ures);
				inv_dct4x4(cvdeq, vres);

				for (uint32_t rr = 0; rr < 4; rr++) {
					uint32_t yy = cy + br * 4u + rr;
					if (yy >= ch) continue;
					for (uint32_t cc = 0; cc < 4; cc++) {
						uint32_t xx = cx + bc * 4u + cc;
						if (xx >= cw) continue;
						uint8_t pu = pred_u[(br * 4u + rr) * 8u + (bc * 4u + cc)];
						uint8_t pv = pred_vp[(br * 4u + rr) * 8u + (bc * 4u + cc)];
						pad.u[yy * pad.stride_uv + xx] = clamp255_i32((int32_t)pu + (int32_t)ures[(int)rr * 4 + (int)cc]);
						pad.v[yy * pad.stride_uv + xx] = clamp255_i32((int32_t)pv + (int32_t)vres[(int)rr * 4 + (int)cc]);
					}
				}
			}
		}
	}

	if (apply_loopfilter) {
		if (vp8_loopfilter_apply_keyframe(&pad, decoded) != 0) {
			yuv420_free(&pad);
			return -1;
		}
	}

	// Crop padded reconstruction down to the visible frame size.
	Yuv420Image cropped;
	if (yuv420_alloc(&cropped, kf->width, kf->height) != 0) {
		yuv420_free(&pad);
		return -1;
	}
	for (uint32_t yy = 0; yy < cropped.height; yy++) {
		memcpy(&cropped.y[yy * cropped.stride_y], &pad.y[yy * pad.stride_y], cropped.width);
	}
	uint32_t cw_out = (cropped.width + 1u) / 2u;
	uint32_t ch_out = (cropped.height + 1u) / 2u;
	for (uint32_t yy = 0; yy < ch_out; yy++) {
		memcpy(&cropped.u[yy * cropped.stride_uv], &pad.u[yy * pad.stride_uv], cw_out);
		memcpy(&cropped.v[yy * cropped.stride_uv], &pad.v[yy * pad.stride_uv], cw_out);
	}

	yuv420_free(&pad);
	*out = cropped;
	return 0;
}

int vp8_reconstruct_keyframe_yuv(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out) {
	return vp8_reconstruct_keyframe_yuv_internal(kf, decoded, out, 0);
}

int vp8_reconstruct_keyframe_yuv_filtered(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out) {
	return vp8_reconstruct_keyframe_yuv_internal(kf, decoded, out, 1);
}
