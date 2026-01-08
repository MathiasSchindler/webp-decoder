#include "enc_vp8_eob.h"

#include "enc_pad.h"

#include "../enc-m02_vp8_bitwriter/enc_bool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NUM_DCT_TOKENS 12

static const uint8_t coeff_update_probs[4][8][3][NUM_DCT_TOKENS - 1] =
#include "../m05_tokens/vp8_tokens_tables_coeff_update_probs.inc"
;

static const uint8_t default_coeff_probs[4][8][3][NUM_DCT_TOKENS - 1] =
#include "../m05_tokens/vp8_tokens_tables_default_coeff_probs.inc"
;

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

static void enc_part0_for_grid(EncBoolEncoder* e, uint32_t mb_total) {
	// Match decoder parse order in src/m05_tokens/vp8_tokens.c.
	enc_bool_put(e, 128, 0);        // color_space
	enc_bool_put(e, 128, 0);        // clamping_type

	enc_bool_put(e, 128, 0);        // segmentation_enabled

	enc_bool_put(e, 128, 0);        // lf_use_simple
	enc_bool_put_literal(e, 0, 6);  // lf_level
	enc_bool_put_literal(e, 0, 3);  // lf_sharpness
	enc_bool_put(e, 128, 0);        // lf_delta_enabled

	enc_bool_put_literal(e, 0, 2);  // log2_partitions = 0 => 1 token partition

	enc_bool_put_literal(e, 0, 7);  // q_index
	for (int i = 0; i < 5; i++) enc_bool_put(e, 128, 0); // all dq=0

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

	// Macroblock prediction records.
	// ymode = DC_PRED: bits 1,0,0 with probs {145,156,163}
	// uv_mode = DC_PRED: bit 0 with prob 142
	for (uint32_t i = 0; i < mb_total; i++) {
		enc_bool_put(e, 145, 1);
		enc_bool_put(e, 156, 0);
		enc_bool_put(e, 163, 0);
		enc_bool_put(e, 142, 0);
	}
}

static void enc_tokens_for_grid(EncBoolEncoder* e, uint32_t mb_total) {
	// All blocks are immediate EOB; because every block has has_coeff=0, contexts stay 0.
	const uint8_t p_y2 = default_coeff_probs[1][0][0][0];
	const uint8_t p_y = default_coeff_probs[0][1][0][0];
	const uint8_t p_uv = default_coeff_probs[2][0][0][0];

	for (uint32_t mb = 0; mb < mb_total; mb++) {
		enc_bool_put(e, p_y2, 0);              // Y2 (1)
		for (int i = 0; i < 16; i++) enc_bool_put(e, p_y, 0);   // Y (16)
		for (int i = 0; i < 4; i++) enc_bool_put(e, p_uv, 0);   // U (4)
		for (int i = 0; i < 4; i++) enc_bool_put(e, p_uv, 0);   // V (4)
	}
}

int enc_vp8_build_keyframe_dc_eob(uint32_t width, uint32_t height, uint8_t** out_payload, size_t* out_size) {
	if (!out_payload || !out_size) {
		errno = EINVAL;
		return -1;
	}
	*out_payload = NULL;
	*out_size = 0;

	uint32_t mb_cols = 0, mb_rows = 0;
	if (enc_vp8_mb_grid(width, height, &mb_cols, &mb_rows) != 0) return -1;
	uint64_t mb_total64 = (uint64_t)mb_cols * (uint64_t)mb_rows;
	if (mb_total64 == 0 || mb_total64 > (1u << 20)) {
		errno = EOVERFLOW;
		return -1;
	}
	uint32_t mb_total = (uint32_t)mb_total64;

	EncBoolEncoder p0;
	enc_bool_init(&p0);
	enc_part0_for_grid(&p0, mb_total);
	enc_bool_finish(&p0);
	if (enc_bool_error(&p0)) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	size_t p0_size = enc_bool_size(&p0);
	if (p0_size > 0x7FFFFu) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}

	EncBoolEncoder tok;
	enc_bool_init(&tok);
	enc_tokens_for_grid(&tok, mb_total);
	enc_bool_finish(&tok);
	if (enc_bool_error(&tok)) {
		enc_bool_free(&tok);
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	size_t tok_size = enc_bool_size(&tok);

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
