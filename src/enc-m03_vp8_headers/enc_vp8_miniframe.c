#include "enc_vp8_miniframe.h"

#include "../enc-m02_vp8_bitwriter/enc_bool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Pull in the exact same RFC-aligned tables the decoder uses.
// These are not "decoder logic"; they are static probability tables.
// We reuse them to guarantee arithmetic-coder probability alignment.

// VP8 has 12 coefficient tokens; probabilities cover 11 internal nodes.
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
	// RFC 6386: 3-byte frame tag, little-endian 24-bit value:
	// bit 0: frame_type (0=key frame)
	// bits 1-3: version
	// bit 4: show_frame
	// bits 5-23: first_partition_len (19 bits)
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
	// Keyframe header (uncompressed):
	// 0x9d 0x01 0x2a, then 16-bit width/height fields with 2-bit scaling.
	dst7[0] = 0x9Du;
	dst7[1] = 0x01u;
	dst7[2] = 0x2Au;
	uint16_t wfield = (uint16_t)(width & 0x3FFFu);
	uint16_t hfield = (uint16_t)(height & 0x3FFFu);
	write_u16le(&dst7[3], wfield);
	write_u16le(&dst7[5], hfield);
}

static void enc_part0_minimal(EncBoolEncoder* e) {
	// Must match the decoder's parse order in src/m05_tokens/vp8_tokens.c.
	//
	// Key-frame-only: color_space and clamping_type.
	enc_bool_put(e, 128, 0);
	enc_bool_put(e, 128, 0);

	// Segmentation: disabled.
	enc_bool_put(e, 128, 0);

	// Loop filter.
	enc_bool_put(e, 128, 0);            // lf_use_simple
	enc_bool_put_literal(e, 0, 6);      // lf_level
	enc_bool_put_literal(e, 0, 3);      // lf_sharpness
	enc_bool_put(e, 128, 0);            // lf_delta_enabled

	// Token partitions: log2_partitions=0 => total_partitions=1.
	enc_bool_put_literal(e, 0, 2);

	// Quantization.
	enc_bool_put_literal(e, 0, 7);      // q_index
	// y1_dc_delta_q, y2_dc_delta_q, y2_ac_delta_q, uv_dc_delta_q, uv_ac_delta_q
	for (int i = 0; i < 5; i++) {
		enc_bool_put(e, 128, 0); // decode_q_delta: flag=0 => delta=0
	}

	// Key-frame: refresh_entropy_probs (decoder ignores value but consumes 1 bit).
	enc_bool_put(e, 128, 0);

	// Token probability updates: output 0 for every update flag.
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 8; j++) {
			for (int k = 0; k < 3; k++) {
				for (int t = 0; t < (NUM_DCT_TOKENS - 1); t++) {
					enc_bool_put(e, coeff_update_probs[i][j][k][t], 0);
				}
			}
		}
	}

	// mb_no_skip_coeff: 0 => no per-mb skip_coeff flags.
	enc_bool_put(e, 128, 0);

	// Macroblock prediction records for a single MB:
	// ymode = DC_PRED using kf_ymode_tree with kf_ymode_prob = {145,156,163,128}
	// Tree walk for DC_PRED yields bits: 1 (node0), 0 (node2), 0 (node4)
	enc_bool_put(e, 145, 1);
	enc_bool_put(e, 156, 0);
	enc_bool_put(e, 163, 0);

	// uv_mode = DC_PRED using uv_mode_prob = {142,114,183}
	// Tree root left is DC_PRED => bit 0
	enc_bool_put(e, 142, 0);
}

static void enc_token_all_eob(EncBoolEncoder* e) {
	// Token partition for a single MB (has_y2=1 for DC_PRED):
	// Y2: plane=1, band=0, ctx=0
	// Y: 16 blocks, plane=0, first_coeff=1 => band=1, ctx=0
	// U: 4 blocks, plane=2, band=0, ctx=0
	// V: 4 blocks, plane=2, band=0, ctx=0
	//
	// Each block encodes an immediate EOB token, which is a single bool at the root
	// of coeff_tree: bit=0 with probability probs[0].

	uint8_t p_y2 = default_coeff_probs[1][0][0][0];
	uint8_t p_y = default_coeff_probs[0][1][0][0];
	uint8_t p_uv = default_coeff_probs[2][0][0][0];

	// Y2
	enc_bool_put(e, p_y2, 0);
	// Y (16)
	for (int i = 0; i < 16; i++) enc_bool_put(e, p_y, 0);
	// U (4)
	for (int i = 0; i < 4; i++) enc_bool_put(e, p_uv, 0);
	// V (4)
	for (int i = 0; i < 4; i++) enc_bool_put(e, p_uv, 0);
}

int enc_vp8_build_minikeyframe_16x16(uint8_t** out_payload, size_t* out_size) {
	if (!out_payload || !out_size) {
		errno = EINVAL;
		return -1;
	}
	*out_payload = NULL;
	*out_size = 0;

	// Build partition 0.
	EncBoolEncoder p0;
	enc_bool_init(&p0);
	enc_part0_minimal(&p0);
	enc_bool_finish(&p0);
	if (enc_bool_error(&p0)) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	const uint8_t* p0_data = enc_bool_data(&p0);
	size_t p0_size = enc_bool_size(&p0);
	if (p0_size > 0x7FFFFu) {
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}

	// Build token partition.
	EncBoolEncoder tok;
	enc_bool_init(&tok);
	enc_token_all_eob(&tok);
	enc_bool_finish(&tok);
	if (enc_bool_error(&tok)) {
		enc_bool_free(&tok);
		enc_bool_free(&p0);
		errno = EINVAL;
		return -1;
	}
	const uint8_t* tok_data = enc_bool_data(&tok);
	size_t tok_size = enc_bool_size(&tok);

	// Assemble VP8 payload.
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
	write_keyframe_start_code_and_dims(&buf[3], 16, 16);
	memcpy(&buf[uncompressed], p0_data, p0_size);
	memcpy(&buf[uncompressed + p0_size], tok_data, tok_size);

	enc_bool_free(&tok);
	enc_bool_free(&p0);

	*out_payload = buf;
	*out_size = total;
	return 0;
}
