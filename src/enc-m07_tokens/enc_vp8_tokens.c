#include "enc_vp8_tokens.h"

#include "../enc-m02_vp8_bitwriter/enc_bool.h"
#include "../enc-m04_yuv/enc_pad.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// VP8 has 12 coefficient tokens; probabilities cover 11 internal nodes.
#define NUM_DCT_TOKENS 12

// Pull in the same RFC-aligned tables as the decoder uses.
static const uint8_t coeff_update_probs[4][8][3][NUM_DCT_TOKENS - 1] =
#include "../m05_tokens/vp8_tokens_tables_coeff_update_probs.inc"
;

static const uint8_t default_coeff_probs[4][8][3][NUM_DCT_TOKENS - 1] =
#include "../m05_tokens/vp8_tokens_tables_default_coeff_probs.inc"
;

typedef enum {
	VP8_I16_DC_PRED = 0,
	VP8_I16_V_PRED = 1,
	VP8_I16_H_PRED = 2,
	VP8_I16_TM_PRED = 3,
	VP8_B_PRED = 4,
} intra_mbmode;

typedef enum {
	B_DC_PRED = 0,
	B_TM_PRED,
	B_VE_PRED,
	B_HE_PRED,
	B_LD_PRED,
	B_RD_PRED,
	B_VR_PRED,
	B_VL_PRED,
	B_HD_PRED,
	B_HU_PRED,
	num_intra_bmodes
} intra_bmode;

// Tree indices are even node offsets; leaves are negative symbols.
static const int8_t bmode_tree[2 * (num_intra_bmodes - 1)] = {
	-B_DC_PRED, 2,
	-B_TM_PRED, 4,
	-B_VE_PRED, 6,
	8, 12,
	-B_HE_PRED, 10,
	-B_RD_PRED, -B_VR_PRED,
	-B_LD_PRED, 14,
	-B_VL_PRED, 16,
	-B_HD_PRED, -B_HU_PRED,
};

static const uint8_t kf_bmode_prob[num_intra_bmodes][num_intra_bmodes][num_intra_bmodes - 1] =
#include "../m05_tokens/vp8_tokens_tables_kf_bmode_prob.inc"
;

static intra_bmode mbmode_to_bmode(intra_mbmode m) {
	switch (m) {
		case VP8_I16_DC_PRED: return B_DC_PRED;
		case VP8_I16_V_PRED: return B_VE_PRED;
		case VP8_I16_H_PRED: return B_HE_PRED;
		case VP8_I16_TM_PRED: return B_TM_PRED;
		default: return B_DC_PRED;
	}
}

// Keyframe ymode/uv mode trees/probs: match decoder.
static const int8_t kf_ymode_tree[2 * (5 - 1)] = {
	-4, 2,
	4, 6,
	-0, -1,
	-2, -3,
};
static const uint8_t kf_ymode_prob[5 - 1] = {145, 156, 163, 128};

static const int8_t uv_mode_tree[2 * (4 - 1)] = {
	-0, 2,
	-1, 4,
	-2, -3,
};
static const uint8_t kf_uv_mode_prob[4 - 1] = {142, 114, 183};

typedef enum {
	DCT_0 = 0,
	DCT_1,
	DCT_2,
	DCT_3,
	DCT_4,
	dct_cat1,
	dct_cat2,
	dct_cat3,
	dct_cat4,
	dct_cat5,
	dct_cat6,
	dct_eob,
	num_dct_tokens
} dct_token;

// Tree indices are even node offsets; leaves are negative symbols.
static const int8_t coeff_tree[2 * (num_dct_tokens - 1)] = {
	-dct_eob, 2,
	-DCT_0, 4,
	-DCT_1, 6,
	8, 12,
	-DCT_2, 10,
	-DCT_3, -DCT_4,
	14, 16,
	-dct_cat1, -dct_cat2,
	18, 20,
	-dct_cat3, -dct_cat4,
	-dct_cat5, -dct_cat6,
};

static const uint8_t coeff_bands[16] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7};
static const uint8_t zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

static const uint8_t Pcat1[] = {159, 0};
static const uint8_t Pcat2[] = {165, 145, 0};
static const uint8_t Pcat3[] = {173, 148, 140, 0};
static const uint8_t Pcat4[] = {176, 155, 140, 135, 0};
static const uint8_t Pcat5[] = {180, 157, 141, 134, 130, 0};
static const uint8_t Pcat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

static void write_u16le(uint8_t* dst, uint16_t v) {
	dst[0] = (uint8_t)(v & 0xFFu);
	dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_vp8_frame_tag(uint8_t dst3[3], uint32_t first_partition_len) {
	uint32_t tag = 0;
	uint32_t frame_type = 0;
	uint32_t version = 0;
	uint32_t show_frame = 1;
	uint32_t part = first_partition_len & 0x7FFFFu;
	tag |= (frame_type & 1u) << 0;
	tag |= (version & 7u) << 1;
	tag |= (show_frame & 1u) << 4;
	tag |= (part & 0x7FFFFu) << 5;

	dst3[0] = (uint8_t)(tag & 0xFFu);
	dst3[1] = (uint8_t)((tag >> 8) & 0xFFu);
	dst3[2] = (uint8_t)((tag >> 16) & 0xFFu);
}

static void write_keyframe_start_code_and_dims(uint8_t dst7[7], uint16_t width, uint16_t height) {
	dst7[0] = 0x9Du;
	dst7[1] = 0x01u;
	dst7[2] = 0x2Au;
	uint16_t wfield = (uint16_t)(width & 0x3FFFu);
	uint16_t hfield = (uint16_t)(height & 0x3FFFu);
	write_u16le(&dst7[3], wfield);
	write_u16le(&dst7[5], hfield);
}

static void enc_write_q_delta(EncBoolEncoder* e, int8_t delta) {
	if (delta == 0) {
		enc_bool_put(e, 128, 0);
		return;
	}
	enc_bool_put(e, 128, 1);
	// 4-bit signed magnitude per decoder parse (bool_decode_sint(d, 4)).
	int v = delta;
	if (v < -15) v = -15;
	if (v > 15) v = 15;
	int sign = (v < 0);
	int mag = sign ? -v : v;
	enc_bool_put_literal(e, (uint32_t)mag, 4);
	enc_bool_put(e, 128, sign);
}

static int tree_contains_symbol(const int8_t* tree, int node, int symbol) {
	const int8_t left = tree[node + 0];
	const int8_t right = tree[node + 1];
	if (left <= 0) {
		if (-left == symbol) return 1;
	} else {
		if (tree_contains_symbol(tree, (int)left, symbol)) return 1;
	}
	if (right <= 0) {
		if (-right == symbol) return 1;
	} else {
		if (tree_contains_symbol(tree, (int)right, symbol)) return 1;
	}
	return 0;
}

static void enc_treed_write(EncBoolEncoder* e, const int8_t* tree, const uint8_t* probs, int start_node, int symbol) {
	int node = start_node;
	for (;;) {
		const int8_t left = tree[node + 0];
		const int8_t right = tree[node + 1];
		const uint8_t p = probs[(unsigned)node >> 1];

		int go_right = 0;
		if (left <= 0) {
			go_right = (-left == symbol) ? 0 : 1;
		} else {
			go_right = tree_contains_symbol(tree, (int)left, symbol) ? 0 : 1;
		}

		enc_bool_put(e, p, go_right);
		const int next = go_right ? (int)right : (int)left;
		if (next <= 0) return;
		node = next;
	}
}

static void enc_write_extra(EncBoolEncoder* e, const uint8_t* probs, uint32_t extra) {
	int bits = 0;
	for (const uint8_t* p = probs; *p; ++p) bits++;
	for (int i = bits - 1; i >= 0; --i) {
		int bit = (int)((extra >> (uint32_t)i) & 1u);
		enc_bool_put(e, probs[bits - 1 - i], bit);
	}
}

static dct_token token_for_abs(int abs_value, uint32_t* out_extra, const uint8_t** out_extraprobs) {
	*out_extra = 0;
	*out_extraprobs = NULL;
	if (abs_value <= 0) return DCT_0;
	if (abs_value <= 4) return (dct_token)abs_value; // DCT_1..DCT_4

	// Categories match decoder.
	// cat1: 5..6 (1 bit)
	// cat2: 7..10 (2 bits)
	// cat3: 11..18 (3 bits)
	// cat4: 19..34 (4 bits)
	// cat5: 35..66 (5 bits)
	// cat6: 67.. (11 bits)
	if (abs_value <= 6) {
		*out_extra = (uint32_t)(abs_value - 5);
		*out_extraprobs = Pcat1;
		return dct_cat1;
	}
	if (abs_value <= 10) {
		*out_extra = (uint32_t)(abs_value - 7);
		*out_extraprobs = Pcat2;
		return dct_cat2;
	}
	if (abs_value <= 18) {
		*out_extra = (uint32_t)(abs_value - 11);
		*out_extraprobs = Pcat3;
		return dct_cat3;
	}
	if (abs_value <= 34) {
		*out_extra = (uint32_t)(abs_value - 19);
		*out_extraprobs = Pcat4;
		return dct_cat4;
	}
	if (abs_value <= 66) {
		*out_extra = (uint32_t)(abs_value - 35);
		*out_extraprobs = Pcat5;
		return dct_cat5;
	}
	*out_extra = (uint32_t)(abs_value - 67);
	*out_extraprobs = Pcat6;
	return dct_cat6;
}

static void enc_write_coeff_token(EncBoolEncoder* e, const uint8_t probs[num_dct_tokens - 1], int prev_token_was_zero, int token) {
	int start_node = prev_token_was_zero ? 2 : 0; // skip eob branch when prev token was DCT_0
	enc_treed_write(e, coeff_tree, probs, start_node, token);
}

static int enc_block(EncBoolEncoder* e,
				 const uint8_t coeff_probs_plane[8][3][num_dct_tokens - 1],
				 int first_coeff,
				 uint8_t left_has,
				 uint8_t above_has,
				 const int16_t block[16]) {
	int ctx3 = (int)left_has + (int)above_has;
	int prev_token_was_zero = 0;
	int current_has_coeffs = 0;

	int last_nz = -1;
	for (int i = first_coeff; i < 16; i++) {
		int v = (int)block[zigzag[i]];
		if (v != 0) last_nz = i;
	}

	// All remaining coefficients are 0 => immediate EOB.
	if (last_nz < 0) {
		int band = (int)coeff_bands[first_coeff];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];
		enc_treed_write(e, coeff_tree, probs, 0, dct_eob);
		return 0;
	}

	for (int i = first_coeff; i <= last_nz; i++) {
		int band = (int)coeff_bands[i];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];

		int v = (int)block[zigzag[i]];
		int abs_value = (v < 0) ? -v : v;

		uint32_t extra = 0;
		const uint8_t* extra_probs = NULL;
		dct_token tok = token_for_abs(abs_value, &extra, &extra_probs);

		enc_write_coeff_token(e, probs, prev_token_was_zero, (int)tok);
		if (tok >= dct_cat1 && tok <= dct_cat6) {
			enc_write_extra(e, extra_probs, extra);
		}
		if (abs_value != 0) {
			enc_bool_put(e, 128, v < 0);
			current_has_coeffs = 1;
		}

		if (abs_value == 0) ctx3 = 0;
		else if (abs_value == 1) ctx3 = 1;
		else ctx3 = 2;

		prev_token_was_zero = (tok == DCT_0);
	}

	// If we didn't end exactly at the last coefficient, emit EOB.
	if (last_nz < 15) {
		int i = last_nz + 1;
		int band = (int)coeff_bands[i];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];
		enc_treed_write(e, coeff_tree, probs, 0, dct_eob);
	}

	return current_has_coeffs;
}

static void enc_part0_for_grid(EncBoolEncoder* e,
							  uint32_t mb_cols,
							  uint32_t mb_rows,
							  uint8_t q_index,
							  int8_t y1_dc_delta_q,
							  int8_t y2_dc_delta_q,
							  int8_t y2_ac_delta_q,
							  int8_t uv_dc_delta_q,
							  int8_t uv_ac_delta_q,
							  const uint8_t* y_modes,
							  const uint8_t* uv_modes,
							  const uint8_t* b_modes,
							  const EncVp8LoopFilterParams* lf) {
	// Match decoder parse order in src/m05_tokens/vp8_tokens.c.
	enc_bool_put(e, 128, 0);        // color_space
	enc_bool_put(e, 128, 0);        // clamping_type

	enc_bool_put(e, 128, 0);        // segmentation_enabled

	uint8_t lf_use_simple = lf ? (uint8_t)(lf->use_simple != 0) : 0;
	uint8_t lf_level = lf ? (uint8_t)(lf->level & 63u) : 0;
	uint8_t lf_sharpness = lf ? (uint8_t)(lf->sharpness & 7u) : 0;
	uint8_t lf_delta_enabled = lf ? (uint8_t)(lf->use_lf_delta != 0) : 0;
	enc_bool_put(e, 128, lf_use_simple ? 1 : 0);
	enc_bool_put_literal(e, lf_level, 6);
	enc_bool_put_literal(e, lf_sharpness, 3);
	enc_bool_put(e, 128, lf_delta_enabled ? 1 : 0);
	if (lf_delta_enabled) {
		// For now, we don't support emitting non-zero deltas; keep defaults.
		enc_bool_put(e, 128, 0); // update = 0
	}

	enc_bool_put_literal(e, 0, 2);  // log2_partitions = 0 => 1 token partition

	enc_bool_put_literal(e, (uint32_t)(q_index & 127u), 7);  // q_index
	enc_write_q_delta(e, y1_dc_delta_q);
	enc_write_q_delta(e, y2_dc_delta_q);
	enc_write_q_delta(e, y2_ac_delta_q);
	enc_write_q_delta(e, uv_dc_delta_q);
	enc_write_q_delta(e, uv_ac_delta_q);

	enc_bool_put(e, 128, 0); // refresh_entropy_probs

	// Token prob updates: all 0.
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 8; j++) {
			for (int k = 0; k < 3; k++) {
				for (int t = 0; t < (NUM_DCT_TOKENS - 1); t++) {
					enc_bool_put(e, coeff_update_probs[i][j][k][t], 0);
				}
			}
		}
	}

	enc_bool_put(e, 128, 0); // mb_no_skip_coeff = 0

	// Subblock mode context predictors (only needed for B_PRED).
	intra_bmode* above_bmodes = (intra_bmode*)malloc((size_t)mb_cols * 4u * sizeof(intra_bmode));
	if (!above_bmodes) {
		e->error = 1;
		return;
	}
	for (uint32_t i = 0; i < mb_cols * 4u; i++) above_bmodes[i] = B_DC_PRED;

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		intra_bmode left_bmodes[4] = {B_DC_PRED, B_DC_PRED, B_DC_PRED, B_DC_PRED};
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb_index = mb_r * mb_cols + mb_c;

			int ymode = y_modes ? (int)y_modes[mb_index] : 0;
			int uvmode = uv_modes ? (int)uv_modes[mb_index] : 0;
			if (ymode < 0 || ymode > 4) ymode = 0;
			if (uvmode < 0 || uvmode > 3) uvmode = 0;

			enc_treed_write(e, kf_ymode_tree, kf_ymode_prob, 0, ymode);
			if (ymode == (int)VP8_B_PRED) {
				intra_bmode local[4][4];
				for (int rr = 0; rr < 4; rr++)
					for (int cc = 0; cc < 4; cc++) local[rr][cc] = B_DC_PRED;
				for (int rr = 0; rr < 4; rr++) {
					for (int cc = 0; cc < 4; cc++) {
						intra_bmode A = (rr == 0) ? above_bmodes[mb_c * 4u + (uint32_t)cc] : local[rr - 1][cc];
						intra_bmode L = (cc == 0) ? left_bmodes[rr] : local[rr][cc - 1];
						const uint8_t* probs = kf_bmode_prob[A][L];
						int sym = b_modes ? (int)b_modes[(size_t)mb_index * 16u + (size_t)(rr * 4 + cc)] : (int)B_DC_PRED;
						if (sym < 0 || sym >= (int)num_intra_bmodes) sym = (int)B_DC_PRED;
						enc_treed_write(e, bmode_tree, probs, 0, sym);
						local[rr][cc] = (intra_bmode)sym;
					}
				}
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4u + (uint32_t)cc] = local[3][cc];
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = local[rr][3];
			} else {
				intra_bmode derived = mbmode_to_bmode((intra_mbmode)ymode);
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4u + (uint32_t)cc] = derived;
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = derived;
			}

			enc_treed_write(e, uv_mode_tree, kf_uv_mode_prob, 0, uvmode);
		}
	}

	free(above_bmodes);
}

static void enc_tokens_for_grid(EncBoolEncoder* e,
							uint32_t mb_cols,
							uint32_t mb_rows,
							const uint8_t* y_modes,
							const int16_t* coeffs) {
	uint8_t* above_y = (uint8_t*)calloc((size_t)mb_cols * 4u, 1);
	uint8_t* above_u = (uint8_t*)calloc((size_t)mb_cols * 2u, 1);
	uint8_t* above_v = (uint8_t*)calloc((size_t)mb_cols * 2u, 1);
	uint8_t* above_y2 = (uint8_t*)calloc((size_t)mb_cols, 1);
	uint8_t left_y[4] = {0, 0, 0, 0};
	uint8_t left_u[2] = {0, 0};
	uint8_t left_v[2] = {0, 0};
	uint8_t left_y2_flag = 0;

	if (!above_y || !above_u || !above_v || !above_y2) {
		free(above_y);
		free(above_u);
		free(above_v);
		free(above_y2);
		e->error = 1;
		return;
	}

	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		left_y[0] = left_y[1] = left_y[2] = left_y[3] = 0;
		left_u[0] = left_u[1] = 0;
		left_v[0] = left_v[1] = 0;
		left_y2_flag = 0;

		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			const size_t mb_index = (size_t)mb_r * (size_t)mb_cols + (size_t)mb_c;
			const int16_t* mb = coeffs + mb_index * coeffs_per_mb;

			int ymode = y_modes ? (int)y_modes[mb_index] : 0;
			int has_y2 = (ymode != (int)VP8_B_PRED);

			// Y2
			if (has_y2) {
				uint8_t left_has = left_y2_flag;
				uint8_t above_has = above_y2[mb_c];
				int has = enc_block(e, default_coeff_probs[1], 0, left_has, above_has, mb);
				above_y2[mb_c] = (uint8_t)has;
				left_y2_flag = (uint8_t)has;
			} else {
				// With no Y2 coded, reset contexts as if Y2 were all-zero.
				above_y2[mb_c] = 0;
				left_y2_flag = 0;
			}

			// Y blocks.
			uint8_t y_has[4][4];
			for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) y_has[rr][cc] = 0;

			const int y_plane = has_y2 ? 0 : 3;
			const int first_coeff = has_y2 ? 1 : 0;

			const int16_t* y = mb + 16;
			for (int rr = 0; rr < 4; rr++) {
				for (int cc = 0; cc < 4; cc++) {
					uint8_t left_has = (cc == 0) ? left_y[rr] : y_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_y[mb_c * 4u + (uint32_t)cc] : y_has[rr - 1][cc];
					int has = enc_block(e, default_coeff_probs[y_plane], first_coeff, left_has, above_has, y + (rr * 4 + cc) * 16);
					y_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 4; cc++) above_y[mb_c * 4u + (uint32_t)cc] = y_has[3][cc];
			for (int rr = 0; rr < 4; rr++) left_y[rr] = y_has[rr][3];

			// U blocks (2x2)
			uint8_t u_has[2][2] = {{0, 0}, {0, 0}};
			const int16_t* u = y + (16 * 16);
			for (int rr = 0; rr < 2; rr++) {
				for (int cc = 0; cc < 2; cc++) {
					uint8_t left_has = (cc == 0) ? left_u[rr] : u_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_u[mb_c * 2u + (uint32_t)cc] : u_has[rr - 1][cc];
					int has = enc_block(e, default_coeff_probs[2], 0, left_has, above_has, u + (rr * 2 + cc) * 16);
					u_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 2; cc++) above_u[mb_c * 2u + (uint32_t)cc] = u_has[1][cc];
			for (int rr = 0; rr < 2; rr++) left_u[rr] = u_has[rr][1];

			// V blocks (2x2)
			uint8_t v_has[2][2] = {{0, 0}, {0, 0}};
			const int16_t* v = u + (4 * 16);
			for (int rr = 0; rr < 2; rr++) {
				for (int cc = 0; cc < 2; cc++) {
					uint8_t left_has = (cc == 0) ? left_v[rr] : v_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_v[mb_c * 2u + (uint32_t)cc] : v_has[rr - 1][cc];
					int has = enc_block(e, default_coeff_probs[2], 0, left_has, above_has, v + (rr * 2 + cc) * 16);
					v_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 2; cc++) above_v[mb_c * 2u + (uint32_t)cc] = v_has[1][cc];
			for (int rr = 0; rr < 2; rr++) left_v[rr] = v_has[rr][1];
		}
	}

	free(above_y);
	free(above_u);
	free(above_v);
	free(above_y2);
}

int enc_vp8_build_keyframe_dc_coeffs(uint32_t width,
								uint32_t height,
								uint8_t q_index,
								int8_t y1_dc_delta_q,
								int8_t y2_dc_delta_q,
								int8_t y2_ac_delta_q,
								int8_t uv_dc_delta_q,
								int8_t uv_ac_delta_q,
								const int16_t* coeffs,
								size_t coeffs_count,
								uint8_t** out_payload,
								size_t* out_size) {
	return enc_vp8_build_keyframe_dc_coeffs_ex(width,
											height,
											q_index,
											y1_dc_delta_q,
											y2_dc_delta_q,
											y2_ac_delta_q,
											uv_dc_delta_q,
											uv_ac_delta_q,
											/*lf=*/NULL,
											coeffs,
											coeffs_count,
											out_payload,
											out_size);
}

int enc_vp8_build_keyframe_dc_coeffs_ex(uint32_t width,
								   uint32_t height,
								   uint8_t q_index,
								   int8_t y1_dc_delta_q,
								   int8_t y2_dc_delta_q,
								   int8_t y2_ac_delta_q,
								   int8_t uv_dc_delta_q,
								   int8_t uv_ac_delta_q,
								   const EncVp8LoopFilterParams* lf,
								   const int16_t* coeffs,
								   size_t coeffs_count,
								   uint8_t** out_payload,
								   size_t* out_size) {
	return enc_vp8_build_keyframe_i16_coeffs_ex(width,
											height,
											q_index,
											y1_dc_delta_q,
											y2_dc_delta_q,
											y2_ac_delta_q,
											uv_dc_delta_q,
											uv_ac_delta_q,
											/*y_modes=*/NULL,
											/*uv_modes=*/NULL,
											lf,
											coeffs,
											coeffs_count,
											out_payload,
											out_size);
}

int enc_vp8_build_keyframe_i16_coeffs(uint32_t width,
								 uint32_t height,
								 uint8_t q_index,
								 int8_t y1_dc_delta_q,
								 int8_t y2_dc_delta_q,
								 int8_t y2_ac_delta_q,
								 int8_t uv_dc_delta_q,
								 int8_t uv_ac_delta_q,
								 const uint8_t* y_modes,
								 const uint8_t* uv_modes,
								 const int16_t* coeffs,
								 size_t coeffs_count,
								 uint8_t** out_payload,
								 size_t* out_size) {
	return enc_vp8_build_keyframe_i16_coeffs_ex(width,
											height,
											q_index,
											y1_dc_delta_q,
											y2_dc_delta_q,
											y2_ac_delta_q,
											uv_dc_delta_q,
											uv_ac_delta_q,
											y_modes,
											uv_modes,
											/*lf=*/NULL,
											coeffs,
											coeffs_count,
											out_payload,
											out_size);
}

int enc_vp8_build_keyframe_i16_coeffs_ex(uint32_t width,
									uint32_t height,
									uint8_t q_index,
									int8_t y1_dc_delta_q,
									int8_t y2_dc_delta_q,
									int8_t y2_ac_delta_q,
									int8_t uv_dc_delta_q,
									int8_t uv_ac_delta_q,
									const uint8_t* y_modes,
									const uint8_t* uv_modes,
									const EncVp8LoopFilterParams* lf,
									const int16_t* coeffs,
									size_t coeffs_count,
									uint8_t** out_payload,
									size_t* out_size) {
	return enc_vp8_build_keyframe_intra_coeffs_ex(width,
												height,
												q_index,
												y1_dc_delta_q,
												y2_dc_delta_q,
												y2_ac_delta_q,
												uv_dc_delta_q,
												uv_ac_delta_q,
												y_modes,
												uv_modes,
												/*b_modes=*/NULL,
												lf,
												coeffs,
												coeffs_count,
												out_payload,
												out_size);
}

int enc_vp8_build_keyframe_intra_coeffs(uint32_t width,
								   uint32_t height,
								   uint8_t q_index,
								   int8_t y1_dc_delta_q,
								   int8_t y2_dc_delta_q,
								   int8_t y2_ac_delta_q,
								   int8_t uv_dc_delta_q,
								   int8_t uv_ac_delta_q,
								   const uint8_t* y_modes,
								   const uint8_t* uv_modes,
								   const uint8_t* b_modes,
								   const int16_t* coeffs,
								   size_t coeffs_count,
								   uint8_t** out_payload,
								   size_t* out_size) {
	return enc_vp8_build_keyframe_intra_coeffs_ex(width,
											height,
											q_index,
											y1_dc_delta_q,
											y2_dc_delta_q,
											y2_ac_delta_q,
											uv_dc_delta_q,
											uv_ac_delta_q,
											y_modes,
											uv_modes,
											b_modes,
											/*lf=*/NULL,
											coeffs,
											coeffs_count,
											out_payload,
											out_size);
}

int enc_vp8_build_keyframe_intra_coeffs_ex(uint32_t width,
									  uint32_t height,
									  uint8_t q_index,
									  int8_t y1_dc_delta_q,
									  int8_t y2_dc_delta_q,
									  int8_t y2_ac_delta_q,
									  int8_t uv_dc_delta_q,
									  int8_t uv_ac_delta_q,
									  const uint8_t* y_modes,
									  const uint8_t* uv_modes,
									  const uint8_t* b_modes,
									  const EncVp8LoopFilterParams* lf,
									  const int16_t* coeffs,
									  size_t coeffs_count,
									  uint8_t** out_payload,
									  size_t* out_size) {
	if (!out_payload || !out_size) {
		errno = EINVAL;
		return -1;
	}
	*out_payload = NULL;
	*out_size = 0;
	if (!coeffs) {
		errno = EINVAL;
		return -1;
	}

	uint32_t mb_cols = 0, mb_rows = 0;
	if (enc_vp8_mb_grid(width, height, &mb_cols, &mb_rows) != 0) return -1;
	uint64_t mb_total64 = (uint64_t)mb_cols * (uint64_t)mb_rows;
	if (mb_total64 == 0 || mb_total64 > (1u << 20)) {
		errno = EOVERFLOW;
		return -1;
	}
	uint32_t mb_total = (uint32_t)mb_total64;

	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	if (coeffs_count != (size_t)mb_total * coeffs_per_mb) {
		errno = EINVAL;
		return -1;
	}

	EncBoolEncoder p0;
	enc_bool_init(&p0);
	enc_part0_for_grid(&p0,
							mb_cols,
							mb_rows,
							q_index,
							y1_dc_delta_q,
							y2_dc_delta_q,
							y2_ac_delta_q,
							uv_dc_delta_q,
							uv_ac_delta_q,
							y_modes,
							uv_modes,
							b_modes,
							lf);
	enc_bool_finish(&p0);
	if (enc_bool_error(&p0)) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	const size_t p0_size = enc_bool_size(&p0);
	if (p0_size > 0x7FFFFu) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}

	EncBoolEncoder tok;
	enc_bool_init(&tok);
	enc_tokens_for_grid(&tok, mb_cols, mb_rows, y_modes, coeffs);
	enc_bool_finish(&tok);
	if (enc_bool_error(&tok)) {
		enc_bool_free(&tok);
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	const size_t tok_size = enc_bool_size(&tok);

	const size_t uncompressed = 10;
	size_t total = uncompressed + p0_size + tok_size;
	uint8_t* buf = (uint8_t*)malloc(total);
	if (!buf) {
		enc_bool_free(&tok);
		enc_bool_free(&p0);
		errno = ENOMEM;
		return -1;
	}

	write_vp8_frame_tag(&buf[0], (uint32_t)p0_size);
	write_keyframe_start_code_and_dims(&buf[3], (uint16_t)width, (uint16_t)height);
	memcpy(&buf[uncompressed], enc_bool_data(&p0), p0_size);
	memcpy(&buf[uncompressed + p0_size], enc_bool_data(&tok), tok_size);

	enc_bool_free(&tok);
	enc_bool_free(&p0);

	*out_payload = buf;
	*out_size = total;
	return 0;
}
