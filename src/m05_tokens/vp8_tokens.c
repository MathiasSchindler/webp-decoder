#include "vp8_tokens.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../m02_vp8_header/vp8_header.h"
#include "../m03_bool_decoder/bool_decoder.h"
#include "vp8_tree.h"

// --- Small helpers ---

static uint64_t fnv1a64_init(void) { return 1469598103934665603ull; }
static uint64_t fnv1a64_u32(uint64_t h, uint32_t v) {
	h ^= (uint64_t)(v & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 8) & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 16) & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 24) & 0xffu);
	h *= 1099511628211ull;
	return h;
}
static uint64_t fnv1a64_i32(uint64_t h, int32_t v) { return fnv1a64_u32(h, (uint32_t)v); }

// --- VP8 trees and probabilities (RFC 6386) ---

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

static uint32_t vp8_read_extra(BoolDecoder* d, const uint8_t* p) {
	uint32_t v = 0;
	while (*p) {
		v = (v << 1) | (uint32_t)bool_decode_bool(d, *p);
		p++;
	}
	return v;
}

// Note: these tables are included as raw initializers from .inc files.
// Some IDE parsers (notably IntelliSense) flag `#include` inside an initializer with
// "expected an expression" even though the compiler accepts it.
// Provide a fallback initializer for the IDE only.
#ifdef __INTELLISENSE__
static const uint8_t coeff_update_probs[4][8][3][num_dct_tokens - 1] = {0};
static const uint8_t default_coeff_probs[4][8][3][num_dct_tokens - 1] = {0};
#else
#if defined(DECODER_ULTRA)
static uint8_t coeff_update_probs[4][8][3][num_dct_tokens - 1];

// RLE stream of (value,count) pairs, expanded into coeff_update_probs at runtime.
// This saves ~600 bytes of .rodata in the ultrabinary.
static const unsigned char coeff_update_probs_rle[] = {
	0xff,0x21,0xb0,0x01,0xf6,0x01,0xff,0x09,0xdf,0x01,0xf1,0x01,0xfc,0x01,0xff,0x08,
	0xf9,0x01,0xfd,0x02,0xff,0x09,0xf4,0x01,0xfc,0x01,0xff,0x08,0xea,0x01,0xfe,0x02,
	0xff,0x08,0xfd,0x01,0xff,0x0b,0xf6,0x01,0xfe,0x01,0xff,0x08,0xef,0x01,0xfd,0x01,
	0xfe,0x01,0xff,0x08,0xfe,0x01,0xff,0x01,0xfe,0x01,0xff,0x09,0xf8,0x01,0xfe,0x01,
	0xff,0x08,0xfb,0x01,0xff,0x01,0xfe,0x01,0xff,0x14,0xfd,0x01,0xfe,0x01,0xff,0x08,
	0xfb,0x01,0xfe,0x02,0xff,0x08,0xfe,0x01,0xff,0x01,0xfe,0x01,0xff,0x09,0xfe,0x01,
	0xfd,0x01,0xff,0x01,0xfe,0x01,0xff,0x06,0xfa,0x01,0xff,0x01,0xfe,0x01,0xff,0x01,
	0xfe,0x01,0xff,0x06,0xfe,0x01,0xff,0x2b,0xd9,0x01,0xff,0x0a,0xe1,0x01,0xfc,0x01,
	0xf1,0x01,0xfd,0x01,0xff,0x02,0xfe,0x01,0xff,0x04,0xea,0x01,0xfa,0x01,0xf1,0x01,
	0xfa,0x01,0xfd,0x01,0xff,0x01,0xfd,0x01,0xfe,0x01,0xff,0x04,0xfe,0x01,0xff,0x09,
	0xdf,0x01,0xfe,0x02,0xff,0x08,0xee,0x01,0xfd,0x01,0xfe,0x02,0xff,0x08,0xf8,0x01,
	0xfe,0x01,0xff,0x08,0xf9,0x01,0xfe,0x01,0xff,0x15,0xfd,0x01,0xff,0x09,0xf7,0x01,
	0xfe,0x01,0xff,0x15,0xfd,0x01,0xfe,0x01,0xff,0x08,0xfc,0x01,0xff,0x16,0xfe,0x02,
	0xff,0x08,0xfd,0x01,0xff,0x16,0xfe,0x01,0xfd,0x01,0xff,0x08,0xfa,0x01,0xff,0x0a,
	0xfe,0x01,0xff,0x2b,0xba,0x01,0xfb,0x01,0xfa,0x01,0xff,0x08,0xea,0x01,0xfb,0x01,
	0xf4,0x01,0xfe,0x01,0xff,0x07,0xfb,0x02,0xf3,0x01,0xfd,0x01,0xfe,0x01,0xff,0x01,
	0xfe,0x01,0xff,0x05,0xfd,0x01,0xfe,0x01,0xff,0x08,0xec,0x01,0xfd,0x01,0xfe,0x01,
	0xff,0x08,0xfb,0x01,0xfd,0x02,0xfe,0x02,0xff,0x07,0xfe,0x02,0xff,0x08,0xfe,0x03,
	0xff,0x14,0xfe,0x01,0xff,0x09,0xfe,0x02,0xff,0x09,0xfe,0x01,0xff,0x15,0xfe,0x01,
	0xff,0x78,0xf8,0x01,0xff,0x0a,0xfa,0x01,0xfe,0x01,0xfc,0x01,0xfe,0x01,0xff,0x07,
	0xf8,0x01,0xfe,0x01,0xf9,0x01,0xfd,0x01,0xff,0x08,0xfd,0x02,0xff,0x08,0xf6,0x01,
	0xfd,0x02,0xff,0x08,0xfc,0x01,0xfe,0x01,0xfb,0x01,0xfe,0x02,0xff,0x07,0xfe,0x01,
	0xfc,0x01,0xff,0x08,0xf8,0x01,0xfe,0x01,0xfd,0x01,0xff,0x08,0xfd,0x01,0xff,0x01,
	0xfe,0x02,0xff,0x08,0xfb,0x01,0xfe,0x01,0xff,0x08,0xf5,0x01,0xfb,0x01,0xfe,0x01,
	0xff,0x08,0xfd,0x02,0xfe,0x01,0xff,0x09,0xfb,0x01,0xfd,0x01,0xff,0x08,0xfc,0x01,
	0xfd,0x01,0xfe,0x01,0xff,0x09,0xfe,0x01,0xff,0x0a,0xfc,0x01,0xff,0x09,0xf9,0x01,
	0xff,0x01,0xfe,0x01,0xff,0x0a,0xfe,0x01,0xff,0x0a,0xfd,0x01,0xff,0x08,0xfa,0x01,
	0xff,0x20,0xfe,0x01,0xff,0x15,
};

static void init_coeff_update_probs(void) {
	static uint8_t inited;
	if (inited) return;
	uint8_t* dst = &coeff_update_probs[0][0][0][0];
	const unsigned char* p = coeff_update_probs_rle;
	const unsigned char* end = coeff_update_probs_rle + sizeof(coeff_update_probs_rle);
	while (p < end) {
		unsigned char v = *p++;
		unsigned char n = *p++;
		while (n--) *dst++ = (uint8_t)v;
	}
	inited = 1;
}
#else
static const uint8_t coeff_update_probs[4][8][3][num_dct_tokens - 1] =
#include "vp8_tokens_tables_coeff_update_probs.inc"
;
#endif

static const uint8_t default_coeff_probs[4][8][3][num_dct_tokens - 1] =
#include "vp8_tokens_tables_default_coeff_probs.inc"
;
#endif

// --- Intra mode trees/probs (key frames) ---

typedef enum {
	DC_PRED = 0,
	V_PRED = 1,
	H_PRED = 2,
	TM_PRED = 3,
	B_PRED = 4,
	num_uv_modes = B_PRED,
	num_ymodes
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

static const int8_t kf_ymode_tree[2 * (num_ymodes - 1)] = {
	-B_PRED, 2,
	4, 6,
	-DC_PRED, -V_PRED,
	-H_PRED, -TM_PRED,
};
static const uint8_t kf_ymode_prob[num_ymodes - 1] = {145, 156, 163, 128};

static const int8_t uv_mode_tree[2 * (num_uv_modes - 1)] = {
	-DC_PRED, 2,
	-V_PRED, 4,
	-H_PRED, -TM_PRED,
};
static const uint8_t kf_uv_mode_prob[num_uv_modes - 1] = {142, 114, 183};

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

#ifdef __INTELLISENSE__
static const uint8_t kf_bmode_prob[num_intra_bmodes][num_intra_bmodes][num_intra_bmodes - 1] = {0};
#else
static const uint8_t kf_bmode_prob[num_intra_bmodes][num_intra_bmodes][num_intra_bmodes - 1] =
#include "vp8_tokens_tables_kf_bmode_prob.inc"
;
#endif

static intra_bmode mbmode_to_bmode(intra_mbmode m) {
	switch (m) {
		case DC_PRED: return B_DC_PRED;
		case V_PRED: return B_VE_PRED;
		case H_PRED: return B_HE_PRED;
		case TM_PRED: return B_TM_PRED;
		default: return B_DC_PRED;
	}
}

// --- Coefficient decode ---

typedef struct {
	uint8_t has_y2;
	uint8_t skip_coeff;
	uint8_t segment_id;
	uint8_t ymode;
	uint8_t uv_mode;
} MbInfo;

static int checked_mul_size(size_t a, size_t b, size_t* out) {
	if (!out) return -1;
	if (a == 0 || b == 0) {
		*out = 0;
		return 0;
	}
	if (a > SIZE_MAX / b) return -1;
	*out = a * b;
	return 0;
}

static void* xcalloc_array(size_t nmemb, size_t size) {
	size_t total = 0;
	if (checked_mul_size(nmemb, size, &total) != 0) return NULL;
	return calloc(1, total);
}

static void* xmalloc_array(size_t nmemb, size_t size) {
	size_t total = 0;
	if (checked_mul_size(nmemb, size, &total) != 0) return NULL;
	return malloc(total);
}

static int read_coeff_token(BoolDecoder* d, const uint8_t probs[num_dct_tokens - 1], int prev_token_was_zero) {
	int start_node = prev_token_was_zero ? 2 : 0; // skip eob branch when prev token was DCT_0
	return vp8_treed_read(d, coeff_tree, probs, start_node);
}

static int decode_block(BoolDecoder* d, uint8_t coeff_probs_plane[8][3][num_dct_tokens - 1], int first_coeff,
						uint8_t left_has, uint8_t above_has, uint32_t* io_nonzero_coeffs, uint32_t* io_eob_tokens,
						uint32_t* io_abs_max, int16_t out_block[16]) {
	for (int i = 0; i < 16; i++) out_block[i] = 0;

	int ctx3 = (int)left_has + (int)above_has;
	int prev_token_was_zero = 0;
	int current_has_coeffs = 0;

	for (int i = first_coeff; i < 16; i++) {
		int band = (int)coeff_bands[i];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];

		int token = read_coeff_token(d, probs, prev_token_was_zero);
		if (token == dct_eob) {
			if (io_eob_tokens) (*io_eob_tokens)++;
			break;
		}

		int abs_value = 0;
		if (token == DCT_0) {
			abs_value = 0;
		} else if (token <= DCT_4) {
			abs_value = token; // 1..4
		} else {
			static const int cat_base[6] = {5, 7, 11, 19, 35, 67};
			int cat = token - dct_cat1;
			uint32_t extra = 0;
			switch (token) {
				case dct_cat1: extra = vp8_read_extra(d, Pcat1); break;
				case dct_cat2: extra = vp8_read_extra(d, Pcat2); break;
				case dct_cat3: extra = vp8_read_extra(d, Pcat3); break;
				case dct_cat4: extra = vp8_read_extra(d, Pcat4); break;
				case dct_cat5: extra = vp8_read_extra(d, Pcat5); break;
				case dct_cat6: extra = vp8_read_extra(d, Pcat6); break;
				default: extra = 0; break;
			}
			abs_value = cat_base[cat] + (int)extra;
		}

		if (abs_value != 0) {
			int sign = bool_decode_bool(d, 128);
			int v = sign ? -abs_value : abs_value;
			out_block[zigzag[i]] = (int16_t)v;
			current_has_coeffs = 1;
			if (io_nonzero_coeffs) (*io_nonzero_coeffs)++;
			uint32_t absu = (uint32_t)abs_value;
			if (io_abs_max && absu > *io_abs_max) *io_abs_max = absu;
		}

		if (abs_value == 0) ctx3 = 0;
		else if (abs_value == 1) ctx3 = 1;
		else ctx3 = 2;

		prev_token_was_zero = (token == DCT_0);
	}

	return current_has_coeffs;
}

static int decode_all_coeffs_keyframe(ByteSpan vp8_payload, const Vp8KeyFrameHeader* kf, uint8_t total_partitions,
					  const MbInfo* mbs, uint32_t mb_cols, uint32_t mb_rows, Vp8CoeffStats* out,
					  Vp8DecodedFrame* frame, uint64_t* io_hash) {
	if (total_partitions != 1) {
		errno = ENOTSUP;
		return -1;
	}

	const size_t uncompressed = 10;
	size_t token_off = uncompressed + (size_t)kf->first_partition_len;
	// No size table when total_partitions==1.
	if (vp8_payload.size < token_off) {
		errno = EINVAL;
		return -1;
	}
	ByteSpan token_part = {
		.data = vp8_payload.data + token_off,
		.size = vp8_payload.size - token_off,
	};
	out->token_part_size_bytes = (uint32_t)token_part.size;
	BoolDecoder d;
	if (bool_decoder_init(&d, token_part) != 0) return -1;

	// Initialize coefficient probabilities (defaults, then apply updates during header parse).
	// For now (single-frame stills), we decode using probabilities that were already updated
	// during header parsing and stored in a static buffer.
	//
	// We pass them via a global static to keep the interface small.
	extern uint8_t g_coeff_probs[4][8][3][num_dct_tokens - 1];

	uint8_t* above_y = NULL;
	uint8_t* above_u = NULL;
	uint8_t* above_v = NULL;
	uint8_t* above_y2 = NULL;
	uint8_t left_y[4] = {0, 0, 0, 0};
	uint8_t left_u[2] = {0, 0};
	uint8_t left_v[2] = {0, 0};
	uint8_t left_y2_flag = 0;

	above_y = (uint8_t*)xcalloc_array((size_t)mb_cols * 4u, sizeof(uint8_t));
	above_u = (uint8_t*)xcalloc_array((size_t)mb_cols * 2u, sizeof(uint8_t));
	above_v = (uint8_t*)xcalloc_array((size_t)mb_cols * 2u, sizeof(uint8_t));
	above_y2 = (uint8_t*)xcalloc_array((size_t)mb_cols, sizeof(uint8_t));
	if (!above_y || !above_u || !above_v || !above_y2) {
		free(above_y);
		free(above_u);
		free(above_v);
		free(above_y2);
		errno = ENOMEM;
		return -1;
	}

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		left_y[0] = left_y[1] = left_y[2] = left_y[3] = 0;
		left_u[0] = left_u[1] = 0;
		left_v[0] = left_v[1] = 0;
		left_y2_flag = 0;

		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb_index = mb_r * mb_cols + mb_c;
			MbInfo info = mbs[mb_index];
			int mb_has_coeff = 0;

			int16_t block[16];
			int16_t* dst = NULL;

			// Y2
			if (info.has_y2) {
				out->blocks_total_y2++;
				uint8_t left_has = left_y2_flag;
				uint8_t above_has = above_y2[mb_c];
				int has = 0;
				if (!info.skip_coeff) {
					has = decode_block(&d, g_coeff_probs[1], 0, left_has, above_has, &out->coeff_nonzero_total,
					                 &out->coeff_eob_tokens, &out->coeff_abs_max, block);
					dst = frame ? (frame->coeff_y2 + (size_t)mb_index * 16u) : NULL;
					for (int i = 0; i < 16; i++) {
						*io_hash = fnv1a64_i32(*io_hash, block[i]);
						if (dst) dst[i] = block[i];
					}
				} else {
					dst = frame ? (frame->coeff_y2 + (size_t)mb_index * 16u) : NULL;
					for (int i = 0; i < 16; i++) {
						*io_hash = fnv1a64_i32(*io_hash, 0);
						if (dst) dst[i] = 0;
					}
				}
				if (has) out->blocks_nonzero_y2++;
				if (has) mb_has_coeff = 1;
				above_y2[mb_c] = (uint8_t)has;
				left_y2_flag = (uint8_t)has;
			} else {
				dst = frame ? (frame->coeff_y2 + (size_t)mb_index * 16u) : NULL;
				if (dst) {
					for (int i = 0; i < 16; i++) dst[i] = 0;
				}
			}

			// Y blocks
			uint8_t y_has[4][4];
			for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) y_has[rr][cc] = 0;

			int y_plane = info.has_y2 ? 0 : 3;
			int first_coeff = info.has_y2 ? 1 : 0;

			for (int rr = 0; rr < 4; rr++) {
				for (int cc = 0; cc < 4; cc++) {
					out->blocks_total_y++;
					uint8_t left_has = (cc == 0) ? left_y[rr] : y_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_y[mb_c * 4 + cc] : y_has[rr - 1][cc];
					int has = 0;
					if (!info.skip_coeff) {
						has = decode_block(&d, g_coeff_probs[y_plane], first_coeff, left_has, above_has,
						                 &out->coeff_nonzero_total, &out->coeff_eob_tokens, &out->coeff_abs_max, block);
						size_t blk = (size_t)mb_index * 16u + (size_t)(rr * 4 + cc);
						dst = frame ? (frame->coeff_y + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, block[i]);
							if (dst) dst[i] = block[i];
						}
					} else {
						size_t blk = (size_t)mb_index * 16u + (size_t)(rr * 4 + cc);
						dst = frame ? (frame->coeff_y + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, 0);
							if (dst) dst[i] = 0;
						}
					}
					if (has) out->blocks_nonzero_y++;
					if (has) mb_has_coeff = 1;
					y_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 4; cc++) {
				above_y[mb_c * 4 + cc] = y_has[3][cc];
			}
			for (int rr = 0; rr < 4; rr++) {
				left_y[rr] = y_has[rr][3];
			}

			// U blocks (2x2)
			uint8_t u_has[2][2] = {{0, 0}, {0, 0}};
			for (int rr = 0; rr < 2; rr++) {
				for (int cc = 0; cc < 2; cc++) {
					out->blocks_total_u++;
					uint8_t left_has = (cc == 0) ? left_u[rr] : u_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_u[mb_c * 2 + cc] : u_has[rr - 1][cc];
					int has = 0;
					if (!info.skip_coeff) {
						has = decode_block(&d, g_coeff_probs[2], 0, left_has, above_has, &out->coeff_nonzero_total,
						                 &out->coeff_eob_tokens, &out->coeff_abs_max, block);
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = frame ? (frame->coeff_u + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, block[i]);
							if (dst) dst[i] = block[i];
						}
					} else {
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = frame ? (frame->coeff_u + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, 0);
							if (dst) dst[i] = 0;
						}
					}
					if (has) out->blocks_nonzero_u++;
					if (has) mb_has_coeff = 1;
					u_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 2; cc++) above_u[mb_c * 2 + cc] = u_has[1][cc];
			for (int rr = 0; rr < 2; rr++) left_u[rr] = u_has[rr][1];

			// V blocks (2x2)
			uint8_t v_has[2][2] = {{0, 0}, {0, 0}};
			for (int rr = 0; rr < 2; rr++) {
				for (int cc = 0; cc < 2; cc++) {
					out->blocks_total_v++;
					uint8_t left_has = (cc == 0) ? left_v[rr] : v_has[rr][cc - 1];
					uint8_t above_has = (rr == 0) ? above_v[mb_c * 2 + cc] : v_has[rr - 1][cc];
					int has = 0;
					if (!info.skip_coeff) {
						has = decode_block(&d, g_coeff_probs[2], 0, left_has, above_has, &out->coeff_nonzero_total,
						                 &out->coeff_eob_tokens, &out->coeff_abs_max, block);
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = frame ? (frame->coeff_v + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, block[i]);
							if (dst) dst[i] = block[i];
						}
					} else {
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = frame ? (frame->coeff_v + blk * 16u) : NULL;
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, 0);
							if (dst) dst[i] = 0;
						}
					}
					if (has) out->blocks_nonzero_v++;
					if (has) mb_has_coeff = 1;
					v_has[rr][cc] = (uint8_t)has;
				}
			}
			for (int cc = 0; cc < 2; cc++) above_v[mb_c * 2 + cc] = v_has[1][cc];
			for (int rr = 0; rr < 2; rr++) left_v[rr] = v_has[rr][1];

			if (frame && frame->has_coeff) frame->has_coeff[mb_index] = (uint8_t)(mb_has_coeff != 0);
		}
	}

	out->token_part_bytes_used = (uint32_t)bool_decoder_bytes_used(&d);
	if (out->token_part_bytes_used > out->token_part_size_bytes) {
		errno = EINVAL;
		return -1;
	}
	out->token_overread = (uint8_t)(bool_decoder_overread(&d) != 0);
	out->token_overread_bytes = bool_decoder_overread_bytes(&d);

	free(above_y);
	free(above_u);
	free(above_v);
	free(above_y2);

	return 0;
}

// Global coeff prob table for the current key frame.
uint8_t g_coeff_probs[4][8][3][num_dct_tokens - 1];

static void init_coeff_probs_defaults(void) {
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 8; j++)
			for (int k = 0; k < 3; k++)
				for (int t = 0; t < (num_dct_tokens - 1); t++) g_coeff_probs[i][j][k][t] = default_coeff_probs[i][j][k][t];
}

static void update_coeff_probs(BoolDecoder* d) {
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 8; j++) {
			for (int k = 0; k < 3; k++) {
				for (int t = 0; t < (num_dct_tokens - 1); t++) {
					if (bool_decode_bool(d, coeff_update_probs[i][j][k][t])) {
						g_coeff_probs[i][j][k][t] = (uint8_t)bool_decode_literal(d, 8);
					}
				}
			}
		}
	}
}

// --- Frame header parse through macroblock data ---

static int8_t decode_q_delta(BoolDecoder* d) {
	if (bool_decode_bool(d, 128) == 0) return 0;
	int32_t v = bool_decode_sint(d, 4);
	if (v < -128) v = -128;
	if (v > 127) v = 127;
	return (int8_t)v;
}

void vp8_decoded_frame_free(Vp8DecodedFrame* f) {
	if (!f) return;
	free(f->segment_id);
	free(f->skip_coeff);
	free(f->has_coeff);
	free(f->ymode);
	free(f->uv_mode);
	free(f->bmode);
	free(f->coeff_y2);
	free(f->coeff_y);
	free(f->coeff_u);
	free(f->coeff_v);
	*f = (Vp8DecodedFrame){0};
}

int vp8_decode_decoded_frame(ByteSpan vp8_payload, Vp8DecodedFrame* out) {
	if (!out) return -1;
	*out = (Vp8DecodedFrame){0};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (!kf.is_key_frame) {
		errno = ENOTSUP;
		return -1;
	}

	uint32_t mb_cols = (kf.width + 15u) / 16u;
	uint32_t mb_rows = (kf.height + 15u) / 16u;
	uint32_t mb_total = mb_cols * mb_rows;
	out->mb_cols = mb_cols;
	out->mb_rows = mb_rows;
	out->mb_total = mb_total;
	out->stats.mb_cols = mb_cols;
	out->stats.mb_rows = mb_rows;
	out->stats.mb_total = mb_total;

	// Guard against overflow/DoS in allocations.
	if (mb_cols == 0 || mb_rows == 0) {
		errno = EINVAL;
		return -1;
	}
	if (mb_total / mb_cols != mb_rows) {
		errno = EINVAL;
		return -1;
	}
	if (mb_total > 1u << 20) {
		errno = EINVAL;
		return -1;
	}

	out->segment_id = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
	out->skip_coeff = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
	out->has_coeff = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
	out->ymode = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
	out->uv_mode = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
	out->bmode = (uint8_t*)xcalloc_array((size_t)mb_total * 16u, sizeof(uint8_t));
	out->coeff_y2 = (int16_t*)xcalloc_array((size_t)mb_total * 16u, sizeof(int16_t));
	out->coeff_y = (int16_t*)xcalloc_array((size_t)mb_total * 16u * 16u, sizeof(int16_t));
	out->coeff_u = (int16_t*)xcalloc_array((size_t)mb_total * 4u * 16u, sizeof(int16_t));
	out->coeff_v = (int16_t*)xcalloc_array((size_t)mb_total * 4u * 16u, sizeof(int16_t));
	if (!out->segment_id || !out->skip_coeff || !out->has_coeff || !out->ymode || !out->uv_mode || !out->bmode || !out->coeff_y2 ||
	    !out->coeff_y || !out->coeff_u || !out->coeff_v) {
		vp8_decoded_frame_free(out);
		errno = ENOMEM;
		return -1;
	}

	const size_t uncompressed = 10;
	if (vp8_payload.size < uncompressed + (size_t)kf.first_partition_len) {
		errno = EINVAL;
		return -1;
	}
	ByteSpan part0 = {vp8_payload.data + uncompressed, kf.first_partition_len};
	out->stats.part0_size_bytes = (uint32_t)part0.size;
	BoolDecoder d;
	if (bool_decoder_init(&d, part0) != 0) return -1;

	// Key-frame-only: color_space and clamping_type.
	(void)bool_decode_bool(&d, 128);
	(void)bool_decode_bool(&d, 128);

	// Segmentation
	int segmentation_enabled = bool_decode_bool(&d, 128);
	out->segmentation_enabled = (uint8_t)(segmentation_enabled != 0);
	out->segmentation_abs = 0;
	for (int i = 0; i < 4; i++) out->seg_quant_idx[i] = 0;
	for (int i = 0; i < 4; i++) out->seg_lf_level[i] = 0;
	int update_mb_segmentation_map = 0;
	uint8_t mb_segment_tree_probs[3] = {255, 255, 255};
	if (segmentation_enabled) {
		update_mb_segmentation_map = bool_decode_bool(&d, 128);
		int update_segment_feature_data = bool_decode_bool(&d, 128);
		if (update_segment_feature_data) {
			int segment_feature_mode = bool_decode_bool(&d, 128);
			// RFC 6386 (update_segmentation table): segment_feature_mode == 0 => delta mode, 1 => absolute-value mode.
			out->segmentation_abs = (uint8_t)(segment_feature_mode != 0);
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) {
					int32_t v = bool_decode_sint(&d, 7);
					if (v < -128) v = -128;
					if (v > 127) v = 127;
					out->seg_quant_idx[i] = (int8_t)v;
				}
			}
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) {
					int32_t v = bool_decode_sint(&d, 6);
					if (v < -128) v = -128;
					if (v > 127) v = 127;
					out->seg_lf_level[i] = (int8_t)v;
				}
			}
		}
		if (update_mb_segmentation_map) {
			for (int i = 0; i < 3; i++) {
				if (bool_decode_bool(&d, 128)) mb_segment_tree_probs[i] = (uint8_t)bool_decode_literal(&d, 8);
			}
		}
	}

	// Loop filter
	out->lf_use_simple = (uint8_t)(bool_decode_bool(&d, 128) != 0);
	out->lf_level = (uint8_t)bool_decode_literal(&d, 6);
	out->lf_sharpness = (uint8_t)bool_decode_literal(&d, 3);
	for (int i = 0; i < 4; i++) out->lf_ref_delta[i] = 0;
	for (int i = 0; i < 4; i++) out->lf_mode_delta[i] = 0;
	out->lf_delta_enabled = (uint8_t)(bool_decode_bool(&d, 128) != 0);
	if (out->lf_delta_enabled) {
		int update = bool_decode_bool(&d, 128);
		if (update) {
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) {
					int32_t v = bool_decode_sint(&d, 6);
					if (v < -128) v = -128;
					if (v > 127) v = 127;
					out->lf_ref_delta[i] = (int8_t)v;
				}
			}
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) {
					int32_t v = bool_decode_sint(&d, 6);
					if (v < -128) v = -128;
					if (v > 127) v = 127;
					out->lf_mode_delta[i] = (int8_t)v;
				}
			}
		}
	}

	// Token partitions
	uint8_t log2_partitions = (uint8_t)bool_decode_literal(&d, 2);
	uint8_t total_partitions = (uint8_t)(1u << log2_partitions);
	if (total_partitions > 8) {
		errno = EINVAL;
		return -1;
	}

	// Quantization
	out->q_index = (uint8_t)bool_decode_literal(&d, 7);
	out->y1_dc_delta_q = decode_q_delta(&d);
	out->y2_dc_delta_q = decode_q_delta(&d);
	out->y2_ac_delta_q = decode_q_delta(&d);
	out->uv_dc_delta_q = decode_q_delta(&d);
	out->uv_ac_delta_q = decode_q_delta(&d);

	// Key-frame: refresh_entropy_probs
	(void)bool_decode_bool(&d, 128);

	// Token probability updates (Section 9.9 / 13.4)
	#if defined(DECODER_ULTRA) && !defined(__INTELLISENSE__)
	init_coeff_update_probs();
	#endif
	init_coeff_probs_defaults();
	update_coeff_probs(&d);

	// mb_no_skip_coeff + prob_skip_false
	int mb_no_skip_coeff = bool_decode_bool(&d, 128);
	uint8_t prob_skip_false = 0;
	if (mb_no_skip_coeff) {
		prob_skip_false = (uint8_t)bool_decode_literal(&d, 8);
	}

	// Token partition size table is ignored for now; our test corpus has Total partitions: 1.

	// Macroblock prediction records (partition 0 remainder)
	MbInfo* mbs = (MbInfo*)xcalloc_array(mb_total, sizeof(MbInfo));
	if (!mbs) {
		vp8_decoded_frame_free(out);
		errno = ENOMEM;
		return -1;
	}

	// Subblock mode context predictors (only needed for B_PRED parsing).
	intra_bmode* above_bmodes = (intra_bmode*)xmalloc_array((size_t)mb_cols * 4u, sizeof(intra_bmode));
	if (!above_bmodes) {
		free(mbs);
		vp8_decoded_frame_free(out);
		errno = ENOMEM;
		return -1;
	}
	for (uint32_t i = 0; i < mb_cols * 4; i++) above_bmodes[i] = B_DC_PRED;

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		intra_bmode left_bmodes[4] = {B_DC_PRED, B_DC_PRED, B_DC_PRED, B_DC_PRED};
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb_index = mb_r * mb_cols + mb_c;
			uint8_t seg_id = 0;

			if (segmentation_enabled && update_mb_segmentation_map) {
				static const int8_t mb_segment_tree[2 * (4 - 1)] = {2, 4, 0, -1, -2, -3};
				seg_id = (uint8_t)vp8_treed_read(&d, mb_segment_tree, mb_segment_tree_probs, 0);
			}
			mbs[mb_index].segment_id = seg_id;
			out->segment_id[mb_index] = seg_id;

			uint8_t skip_coeff = 0;
			if (mb_no_skip_coeff) {
				skip_coeff = (uint8_t)bool_decode_bool(&d, prob_skip_false);
			}
			mbs[mb_index].skip_coeff = skip_coeff;
			out->skip_coeff[mb_index] = skip_coeff;
			if (skip_coeff) out->stats.mb_skip_coeff++;

			intra_mbmode ymode = (intra_mbmode)vp8_treed_read(&d, kf_ymode_tree, kf_ymode_prob, 0);
			mbs[mb_index].ymode = (uint8_t)ymode;
			out->ymode[mb_index] = (uint8_t)ymode;
			if ((unsigned)ymode < 5u) out->stats.ymode_counts[(unsigned)ymode]++;
			if (ymode == B_PRED) {
				out->stats.mb_b_pred++;
				mbs[mb_index].has_y2 = 0;
				intra_bmode local[4][4];
				for (int rr = 0; rr < 4; rr++)
					for (int cc = 0; cc < 4; cc++) local[rr][cc] = B_DC_PRED;
				for (int rr = 0; rr < 4; rr++) {
					for (int cc = 0; cc < 4; cc++) {
						intra_bmode A = (rr == 0) ? above_bmodes[mb_c * 4 + cc] : local[rr - 1][cc];
						intra_bmode L = (cc == 0) ? left_bmodes[rr] : local[rr][cc - 1];
						const uint8_t* probs = kf_bmode_prob[A][L];
						local[rr][cc] = (intra_bmode)vp8_treed_read(&d, bmode_tree, probs, 0);
						out->bmode[(size_t)mb_index * 16u + (size_t)(rr * 4 + cc)] = (uint8_t)local[rr][cc];
						if ((unsigned)local[rr][cc] < 10u) out->stats.bmode_counts[(unsigned)local[rr][cc]]++;
					}
				}
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4 + cc] = local[3][cc];
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = local[rr][3];
			} else {
				mbs[mb_index].has_y2 = 1;
				intra_bmode derived = mbmode_to_bmode(ymode);
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4 + cc] = derived;
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = derived;
				for (int rr = 0; rr < 4; rr++)
					for (int cc = 0; cc < 4; cc++)
						out->bmode[(size_t)mb_index * 16u + (size_t)(rr * 4 + cc)] = (uint8_t)derived;
			}

			unsigned uv_mode = (unsigned)vp8_treed_read(&d, uv_mode_tree, kf_uv_mode_prob, 0);
			mbs[mb_index].uv_mode = (uint8_t)uv_mode;
			out->uv_mode[mb_index] = (uint8_t)uv_mode;
			if (uv_mode < 4u) out->stats.uv_mode_counts[uv_mode]++;
		}
	}

	out->stats.part0_bytes_used = (uint32_t)bool_decoder_bytes_used(&d);
	if (out->stats.part0_bytes_used > out->stats.part0_size_bytes) {
		errno = EINVAL;
		free(above_bmodes);
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	out->stats.part0_overread = (uint8_t)(bool_decoder_overread(&d) != 0);
	out->stats.part0_overread_bytes = bool_decoder_overread_bytes(&d);

	// RFC-aligned internal consistency checks.
	{
		uint32_t ysum = 0;
		for (int i = 0; i < 5; i++) ysum += out->stats.ymode_counts[i];
		if (ysum != mb_total) {
			errno = EINVAL;
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
		uint32_t uvsum = 0;
		for (int i = 0; i < 4; i++) uvsum += out->stats.uv_mode_counts[i];
		if (uvsum != mb_total) {
			errno = EINVAL;
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
		uint32_t bsum = 0;
		for (int i = 0; i < 10; i++) bsum += out->stats.bmode_counts[i];
		if (bsum != out->stats.mb_b_pred * 16u) {
			errno = EINVAL;
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
	}

	uint64_t h = fnv1a64_init();
	if (decode_all_coeffs_keyframe(vp8_payload, &kf, total_partitions, mbs, mb_cols, mb_rows, &out->stats, out, &h) != 0) {
		free(above_bmodes);
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	free(above_bmodes);

	// More internal sanity checks: block totals implied by macroblock structure.
	if (out->stats.blocks_total_y != mb_total * 16u) {
		errno = EINVAL;
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	if (out->stats.blocks_total_u != mb_total * 4u || out->stats.blocks_total_v != mb_total * 4u) {
		errno = EINVAL;
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	if (out->stats.blocks_total_y2 != (mb_total - out->stats.mb_b_pred)) {
		errno = EINVAL;
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	out->stats.coeff_hash_fnv1a64 = h;
	free(mbs);
	return 0;
}

int vp8_decode_coeff_stats(ByteSpan vp8_payload, Vp8CoeffStats* out) {
	if (!out) return -1;
	Vp8DecodedFrame f;
	if (vp8_decode_decoded_frame(vp8_payload, &f) != 0) return -1;
	*out = f.stats;
	vp8_decoded_frame_free(&f);
	return 0;
}
