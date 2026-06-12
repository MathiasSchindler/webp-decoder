#include "vp8_tokens.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "vp8_header.h"
#include "bool_decoder.h"

#define bool_decode_bool bool_decode_bool_inline
#define bool_decode_bit bool_decode_bit_inline
#define bool_decode_literal bool_decode_literal_inline
#define bool_decode_sint bool_decode_sint_inline

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

static const uint8_t coeff_bands[16] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7};

static const uint8_t zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

static inline uint32_t vp8_read_extra_cat(BoolDecoder* d, int cat) {
	switch (cat) {
		case 0: return (uint32_t)bool_decode_bool(d, 159);
		case 1:
			return ((uint32_t)bool_decode_bool(d, 165) << 1) |
			       (uint32_t)bool_decode_bool(d, 145);
		case 2:
			return ((uint32_t)bool_decode_bool(d, 173) << 2) |
			       ((uint32_t)bool_decode_bool(d, 148) << 1) |
			       (uint32_t)bool_decode_bool(d, 140);
		case 3:
			return ((uint32_t)bool_decode_bool(d, 176) << 3) |
			       ((uint32_t)bool_decode_bool(d, 155) << 2) |
			       ((uint32_t)bool_decode_bool(d, 140) << 1) |
			       (uint32_t)bool_decode_bool(d, 135);
		case 4:
			return ((uint32_t)bool_decode_bool(d, 180) << 4) |
			       ((uint32_t)bool_decode_bool(d, 157) << 3) |
			       ((uint32_t)bool_decode_bool(d, 141) << 2) |
			       ((uint32_t)bool_decode_bool(d, 134) << 1) |
			       (uint32_t)bool_decode_bool(d, 130);
		default:
			return ((uint32_t)bool_decode_bool(d, 254) << 10) |
			       ((uint32_t)bool_decode_bool(d, 254) << 9) |
			       ((uint32_t)bool_decode_bool(d, 243) << 8) |
			       ((uint32_t)bool_decode_bool(d, 230) << 7) |
			       ((uint32_t)bool_decode_bool(d, 196) << 6) |
			       ((uint32_t)bool_decode_bool(d, 177) << 5) |
			       ((uint32_t)bool_decode_bool(d, 153) << 4) |
			       ((uint32_t)bool_decode_bool(d, 140) << 3) |
			       ((uint32_t)bool_decode_bool(d, 133) << 2) |
			       ((uint32_t)bool_decode_bool(d, 130) << 1) |
			       (uint32_t)bool_decode_bool(d, 129);
	}
}

// Note: these tables are included as raw initializers from .inc files.
// Some IDE parsers (notably IntelliSense) flag `#include` inside an initializer with
// "expected an expression" even though the compiler accepts it.
// Provide a fallback initializer for the IDE only.
#ifdef __INTELLISENSE__
static const uint8_t coeff_update_probs[4][8][3][num_dct_tokens - 1] = {0};
static const uint8_t default_coeff_probs[4][8][3][num_dct_tokens - 1] = {0};
#else
static const uint8_t coeff_update_probs[4][8][3][num_dct_tokens - 1] =
#include "vp8_tokens_tables_coeff_update_probs.inc"
;

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

static const uint8_t kf_ymode_prob[num_ymodes - 1] = {145, 156, 163, 128};

static const uint8_t kf_uv_mode_prob[num_uv_modes - 1] = {142, 114, 183};

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
	uint32_t bmode_offset;
	uint8_t flags;
	uint8_t segment_id;
	uint8_t ymode;
	uint8_t uv_mode;
} MbInfo;

#define MBINFO_HAS_Y2 1u
#define MBINFO_SKIP_COEFF 2u
#define MBINFO_NO_BMODE_OFFSET UINT32_MAX

static int checked_mul_size(size_t a, size_t b, size_t* out);

typedef struct {
	uint8_t* data;
	uint32_t count;
	uint32_t capacity;
} BPredModeStore;

static void bpred_mode_store_free(BPredModeStore* s) {
	if (!s) return;
	free(s->data);
	*s = (BPredModeStore){0};
}

static int bpred_mode_store_append(BPredModeStore* s, intra_bmode modes[4][4], uint32_t* out_offset) {
	if (!s || !modes || !out_offset) {
		errno = EINVAL;
		return -1;
	}
	if (s->count > (UINT32_MAX / 16u) - 1u) {
		errno = ENOMEM;
		return -1;
	}
	if (s->count == s->capacity) {
		uint32_t new_capacity = s->capacity ? s->capacity * 2u : 64u;
		if (new_capacity < s->capacity || new_capacity > UINT32_MAX / 16u) {
			new_capacity = UINT32_MAX / 16u;
		}
		if (new_capacity <= s->capacity) {
			errno = ENOMEM;
			return -1;
		}
		size_t bytes = 0;
		if (checked_mul_size((size_t)new_capacity, 16u, &bytes) != 0) {
			errno = ENOMEM;
			return -1;
		}
		uint8_t* p = (uint8_t*)realloc(s->data, bytes);
		if (!p) {
			errno = ENOMEM;
			return -1;
		}
		s->data = p;
		s->capacity = new_capacity;
	}
	uint32_t offset = s->count * 16u;
	for (int rr = 0; rr < 4; rr++)
		for (int cc = 0; cc < 4; cc++) s->data[offset + (uint32_t)(rr * 4 + cc)] = (uint8_t)modes[rr][cc];
	s->count++;
	*out_offset = offset;
	return 0;
}

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

typedef struct {
	const uint8_t* buf;
	const uint8_t* end;
	uint32_t value;
	uint32_t range;
	int count;
	uint8_t overread;
	uint32_t overread_bytes;
} BoolDecoderFastState;

static inline BoolDecoderFastState bool_decoder_fast_state_load(const BoolDecoder* d) {
	BoolDecoderFastState s = {
		.buf = d->buf,
		.end = d->end,
		.value = d->value,
		.range = d->range,
		.count = d->count,
		.overread = d->overread,
		.overread_bytes = d->overread_bytes,
	};
	return s;
}

static inline void bool_decoder_fast_state_store(BoolDecoder* d, const BoolDecoderFastState* s) {
	d->buf = s->buf;
	d->value = s->value;
	d->range = (uint8_t)s->range;
	d->count = s->count;
	d->overread = s->overread;
	d->overread_bytes = s->overread_bytes;
}

static inline void bool_fast_refill(BoolDecoderFastState* s) {
	if (BOOL_DECODER_UNLIKELY(s->count >= 0)) {
		if (BOOL_DECODER_LIKELY(s->buf < s->end)) {
			s->value |= (uint32_t)(*s->buf++) << s->count;
		} else {
			s->overread = 1;
			s->overread_bytes++;
		}
		s->count -= 8;
	}
}

static inline int bool_fast_decode_bool(BoolDecoderFastState* s, uint8_t prob) {
	uint32_t range = s->range;
	uint32_t value = s->value;
	uint32_t split = 1u + (((range - 1u) * (uint32_t)prob) >> 8);
	uint32_t bigsplit = split << 8;

	uint32_t bit = value >= bigsplit;
	uint32_t mask = 0u - bit;
	value -= bigsplit & mask;
	range = split + ((range - (split << 1)) & mask);

	uint8_t shift = bool_decoder_norm_shift((uint8_t)range);
	s->range = range << shift;
	s->value = value << shift;
	s->count += shift;
	bool_fast_refill(s);
	return (int)bit;
}

static inline int bool_fast_decode_bit(BoolDecoderFastState* s) {
	uint32_t range = s->range;
	uint32_t value = s->value;
	uint32_t split = (range + 1u) >> 1;
	uint32_t bigsplit = split << 8;

	uint32_t bit = value >= bigsplit;
	uint32_t mask = 0u - bit;
	value -= bigsplit & mask;
	range = split + ((range - (split << 1)) & mask);

	uint8_t shift = bool_decoder_norm_shift((uint8_t)range);
	s->range = range << shift;
	s->value = value << shift;
	s->count += shift;
	bool_fast_refill(s);
	return (int)bit;
}

static inline uint32_t vp8_read_extra_cat_fast(BoolDecoderFastState* s, int cat) {
	switch (cat) {
		case 0: return (uint32_t)bool_fast_decode_bool(s, 159);
		case 1:
			return ((uint32_t)bool_fast_decode_bool(s, 165) << 1) |
			       (uint32_t)bool_fast_decode_bool(s, 145);
		case 2:
			return ((uint32_t)bool_fast_decode_bool(s, 173) << 2) |
			       ((uint32_t)bool_fast_decode_bool(s, 148) << 1) |
			       (uint32_t)bool_fast_decode_bool(s, 140);
		case 3:
			return ((uint32_t)bool_fast_decode_bool(s, 176) << 3) |
			       ((uint32_t)bool_fast_decode_bool(s, 155) << 2) |
			       ((uint32_t)bool_fast_decode_bool(s, 140) << 1) |
			       (uint32_t)bool_fast_decode_bool(s, 135);
		case 4:
			return ((uint32_t)bool_fast_decode_bool(s, 180) << 4) |
			       ((uint32_t)bool_fast_decode_bool(s, 157) << 3) |
			       ((uint32_t)bool_fast_decode_bool(s, 141) << 2) |
			       ((uint32_t)bool_fast_decode_bool(s, 134) << 1) |
			       (uint32_t)bool_fast_decode_bool(s, 130);
		default:
			return ((uint32_t)bool_fast_decode_bool(s, 254) << 10) |
			       ((uint32_t)bool_fast_decode_bool(s, 254) << 9) |
			       ((uint32_t)bool_fast_decode_bool(s, 243) << 8) |
			       ((uint32_t)bool_fast_decode_bool(s, 230) << 7) |
			       ((uint32_t)bool_fast_decode_bool(s, 196) << 6) |
			       ((uint32_t)bool_fast_decode_bool(s, 177) << 5) |
			       ((uint32_t)bool_fast_decode_bool(s, 153) << 4) |
			       ((uint32_t)bool_fast_decode_bool(s, 140) << 3) |
			       ((uint32_t)bool_fast_decode_bool(s, 133) << 2) |
			       ((uint32_t)bool_fast_decode_bool(s, 130) << 1) |
			       (uint32_t)bool_fast_decode_bool(s, 129);
	}
}

static inline void record_coeff_token(Vp8CoeffStats* stats, int token, uint32_t path_bits) {
	stats->coeff_token_counts[token]++;
	stats->coeff_token_reads++;
	stats->coeff_token_path_bits += path_bits;
	if (token == dct_eob) stats->coeff_eob_tokens++;
	else if (token == DCT_0) stats->coeff_zero_tokens++;
	else if (token == DCT_1) stats->coeff_one_tokens++;
}

typedef enum {
	COEFFS_ZERO = 0,
	COEFFS_DC_ONLY = 1,
	COEFFS_HAS_AC = 2,
} EntropyCoeffClass;

static inline uint64_t prof_now_ns(const Vp8EntropyProfile* profile) {
	return profile ? os_monotonic_raw_ns() : 0;
}

static inline void prof_add_ns(uint64_t* dst, uint64_t start) {
	if (dst && start) *dst += os_monotonic_raw_ns() - start;
}

static inline void prof_note_refill(Vp8EntropyProfile* profile, const BoolDecoder* d, const uint8_t* before_buf,
                                    uint32_t before_overread, uint64_t elapsed_ns) {
	if (!profile) return;
	if (d->buf != before_buf || d->overread_bytes != before_overread) {
		profile->bool_refill_events++;
		profile->bool_refill_ns += elapsed_ns;
	}
}

static EntropyCoeffClass entropy_coeffs_classify(const int16_t coeffs[16]) {
	if (coeffs[0] != 0) {
		for (int i = 1; i < 16; i++) {
			if (coeffs[i] != 0) return COEFFS_HAS_AC;
		}
		return COEFFS_DC_ONLY;
	}
	for (int i = 1; i < 16; i++) {
		if (coeffs[i] != 0) return COEFFS_HAS_AC;
	}
	return COEFFS_ZERO;
}

static void entropy_profile_zero(Vp8EntropyProfile* profile) {
	if (!profile) return;
	volatile uint64_t* p = (volatile uint64_t*)(void*)profile;
	for (size_t i = 0; i < sizeof(*profile) / sizeof(uint64_t); i++) p[i] = 0;
}

static inline int read_coeff_token_profiled(BoolDecoder* d,
                                            const uint8_t probs[num_dct_tokens - 1],
                                            int prev_token_was_zero,
                                            Vp8CoeffStats* stats) {
	uint32_t path_bits = 0;
#define READ_TOKEN_BIT(p_) (stats->coeff_bool_calls++, stats->coeff_token_bool_calls++, path_bits++, bool_decode_bool(d, (p_)))
	if (!prev_token_was_zero && !READ_TOKEN_BIT(probs[0])) {
		record_coeff_token(stats, dct_eob, path_bits);
		return dct_eob;
	}
	if (!READ_TOKEN_BIT(probs[1])) {
		record_coeff_token(stats, DCT_0, path_bits);
		return DCT_0;
	}
	if (!READ_TOKEN_BIT(probs[2])) {
		record_coeff_token(stats, DCT_1, path_bits);
		return DCT_1;
	}
	if (!READ_TOKEN_BIT(probs[3])) {
		int token;
		if (!READ_TOKEN_BIT(probs[4])) token = DCT_2;
		else token = READ_TOKEN_BIT(probs[5]) ? DCT_4 : DCT_3;
		record_coeff_token(stats, token, path_bits);
		return token;
	}
	if (!READ_TOKEN_BIT(probs[6])) {
		int token = READ_TOKEN_BIT(probs[7]) ? dct_cat2 : dct_cat1;
		record_coeff_token(stats, token, path_bits);
		return token;
	}
	if (!READ_TOKEN_BIT(probs[8])) {
		int token = READ_TOKEN_BIT(probs[9]) ? dct_cat4 : dct_cat3;
		record_coeff_token(stats, token, path_bits);
		return token;
	}
	int token = READ_TOKEN_BIT(probs[10]) ? dct_cat6 : dct_cat5;
	record_coeff_token(stats, token, path_bits);
	return token;
#undef READ_TOKEN_BIT
}

static inline uint32_t vp8_read_extra_cat_profiled(BoolDecoder* d, int cat, Vp8CoeffStats* stats) {
	static const uint8_t cat_bits[6] = {1, 2, 3, 4, 5, 11};
	stats->coeff_extra_category_counts[cat]++;
	stats->coeff_extra_bits += cat_bits[cat];
	stats->coeff_bool_calls += cat_bits[cat];
	return vp8_read_extra_cat(d, cat);
}

static inline intra_mbmode read_kf_ymode_fast(BoolDecoderFastState* d) {
	if (!bool_fast_decode_bool(d, kf_ymode_prob[0])) return B_PRED;
	if (!bool_fast_decode_bool(d, kf_ymode_prob[1])) {
		return bool_fast_decode_bool(d, kf_ymode_prob[2]) ? V_PRED : DC_PRED;
	}
	return bool_fast_decode_bool(d, kf_ymode_prob[3]) ? TM_PRED : H_PRED;
}

static inline unsigned read_kf_uv_mode_fast(BoolDecoderFastState* d) {
	if (!bool_fast_decode_bool(d, kf_uv_mode_prob[0])) return DC_PRED;
	if (!bool_fast_decode_bool(d, kf_uv_mode_prob[1])) return V_PRED;
	return bool_fast_decode_bool(d, kf_uv_mode_prob[2]) ? TM_PRED : H_PRED;
}

static inline intra_bmode read_kf_bmode_fast(BoolDecoderFastState* d, const uint8_t probs[num_intra_bmodes - 1]) {
	if (!bool_fast_decode_bool(d, probs[0])) return B_DC_PRED;
	if (!bool_fast_decode_bool(d, probs[1])) return B_TM_PRED;
	if (!bool_fast_decode_bool(d, probs[2])) return B_VE_PRED;
	if (!bool_fast_decode_bool(d, probs[3])) {
		if (!bool_fast_decode_bool(d, probs[4])) return B_HE_PRED;
		return bool_fast_decode_bool(d, probs[5]) ? B_VR_PRED : B_RD_PRED;
	}
	if (!bool_fast_decode_bool(d, probs[6])) return B_LD_PRED;
	if (!bool_fast_decode_bool(d, probs[7])) return B_VL_PRED;
	return bool_fast_decode_bool(d, probs[8]) ? B_HU_PRED : B_HD_PRED;
}

static inline uint8_t read_mb_segment_fast(BoolDecoderFastState* d, const uint8_t probs[3]) {
	if (!bool_fast_decode_bool(d, probs[0])) {
		return (uint8_t)(bool_fast_decode_bool(d, probs[1]) ? 1u : 0u);
	}
	return (uint8_t)(bool_fast_decode_bool(d, probs[2]) ? 3u : 2u);
}

static void record_token_overread_loc(Vp8CoeffStats* out,
								 uint32_t mb_index,
								 uint32_t plane,
								 uint32_t block_index,
								 uint32_t coeff_i,
								 uint32_t stage) {
	if (!out) return;
	// Only record the first occurrence.
	if (out->token_overread_mb_index != 0xFFFFFFFFu) return;
	out->token_overread_mb_index = mb_index;
	out->token_overread_plane = plane;
	out->token_overread_block_index = block_index;
	out->token_overread_coeff_i = coeff_i;
	out->token_overread_stage = stage;
}

static int decode_block_fast_state(BoolDecoderFastState* bd,
                                   uint8_t coeff_probs_plane[8][3][num_dct_tokens - 1],
                                   int first_coeff,
                                   uint8_t left_has,
                                   uint8_t above_has,
                                   int16_t out_block[16]) {
	static const int cat_base[6] = {5, 7, 11, 19, 35, 67};
	int ctx3 = (int)left_has + (int)above_has;
	int prev_token_was_zero = 0;
	int current_has_coeffs = 0;

	for (int i = first_coeff; i < 16; i++) {
		int band = (int)coeff_bands[i];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];

		if (BOOL_DECODER_UNLIKELY(!prev_token_was_zero && !bool_fast_decode_bool(bd, probs[0]))) break;
		if (!bool_fast_decode_bool(bd, probs[1])) {
			ctx3 = 0;
			prev_token_was_zero = 1;
			continue;
		}

		int abs_value;
		if (BOOL_DECODER_LIKELY(!bool_fast_decode_bool(bd, probs[2]))) {
			abs_value = 1;
			ctx3 = 1;
		} else {
			if (!bool_fast_decode_bool(bd, probs[3])) {
				if (!bool_fast_decode_bool(bd, probs[4])) {
					abs_value = 2;
				} else {
					abs_value = bool_fast_decode_bool(bd, probs[5]) ? 4 : 3;
				}
			} else {
				int cat;
				if (!bool_fast_decode_bool(bd, probs[6])) {
					cat = bool_fast_decode_bool(bd, probs[7]) ? 1 : 0;
				} else if (!bool_fast_decode_bool(bd, probs[8])) {
					cat = bool_fast_decode_bool(bd, probs[9]) ? 3 : 2;
				} else {
					cat = bool_fast_decode_bool(bd, probs[10]) ? 5 : 4;
				}
				abs_value = cat_base[cat] + (int)vp8_read_extra_cat_fast(bd, cat);
			}
			ctx3 = 2;
		}

		int sign = bool_fast_decode_bit(bd);
		int v = sign ? -abs_value : abs_value;
		out_block[zigzag[i]] = (int16_t)v;
		current_has_coeffs = 1;
		prev_token_was_zero = 0;
	}

	return current_has_coeffs;
}

static int decode_block_profiled(BoolDecoder* d,
                                 uint8_t coeff_probs_plane[8][3][num_dct_tokens - 1],
                                 int first_coeff,
                                 uint8_t left_has,
                                 uint8_t above_has,
                                 int16_t out_block[16],
                                 Vp8CoeffStats* out_stats,
                                 Vp8EntropyProfile* profile,
                                 uint32_t mb_index,
                                 uint32_t plane,
                                 uint32_t block_index) {
	int ctx3 = (int)left_has + (int)above_has;
	int prev_token_was_zero = 0;
	int current_has_coeffs = 0;

	for (int i = first_coeff; i < 16; i++) {
		int band = (int)coeff_bands[i];
		const uint8_t* probs = coeff_probs_plane[band][ctx3];

		const uint8_t* before_buf = d->buf;
		uint32_t before_overread = d->overread_bytes;
		uint64_t t0 = prof_now_ns(profile);
		int token = read_coeff_token_profiled(d, probs, prev_token_was_zero, out_stats);
		uint64_t elapsed = t0 ? (os_monotonic_raw_ns() - t0) : 0;
		if (profile) {
			profile->token_tree_ns += elapsed;
			prof_note_refill(profile, d, before_buf, before_overread, elapsed);
		}
		if (bool_decoder_overread(d)) {
			record_token_overread_loc(out_stats, mb_index, plane, block_index, (uint32_t)i, /*stage=*/0);
		}
		if (token == dct_eob) break;

		int abs_value = 0;
		if (token == DCT_0) {
			abs_value = 0;
		} else if (token <= DCT_4) {
			abs_value = token;
		} else {
			static const int cat_base[6] = {5, 7, 11, 19, 35, 67};
			int cat = token - dct_cat1;
			before_buf = d->buf;
			before_overread = d->overread_bytes;
			t0 = prof_now_ns(profile);
			uint32_t extra = vp8_read_extra_cat_profiled(d, cat, out_stats);
			elapsed = t0 ? (os_monotonic_raw_ns() - t0) : 0;
			if (profile) {
				profile->token_extra_bits_ns += elapsed;
				prof_note_refill(profile, d, before_buf, before_overread, elapsed);
			}
			if (bool_decoder_overread(d)) {
				record_token_overread_loc(out_stats, mb_index, plane, block_index, (uint32_t)i, /*stage=*/1);
			}
			abs_value = cat_base[cat] + (int)extra;
		}

		if (abs_value != 0) {
			out_stats->coeff_bool_calls++;
			out_stats->coeff_sign_bits++;
			before_buf = d->buf;
			before_overread = d->overread_bytes;
			t0 = prof_now_ns(profile);
			int sign = bool_decode_bit(d);
			elapsed = t0 ? (os_monotonic_raw_ns() - t0) : 0;
			if (profile) {
				profile->token_sign_bits_ns += elapsed;
				prof_note_refill(profile, d, before_buf, before_overread, elapsed);
			}
			if (bool_decoder_overread(d)) {
				record_token_overread_loc(out_stats, mb_index, plane, block_index, (uint32_t)i, /*stage=*/2);
			}
			int v = sign ? -abs_value : abs_value;
			out_block[zigzag[i]] = (int16_t)v;
			current_has_coeffs = 1;
			out_stats->coeff_nonzero_total++;
			uint32_t absu = (uint32_t)abs_value;
			if (absu > out_stats->coeff_abs_max) out_stats->coeff_abs_max = absu;
		}

		t0 = prof_now_ns(profile);
		if (abs_value == 0) ctx3 = 0;
		else if (abs_value == 1) ctx3 = 1;
		else ctx3 = 2;
		if (profile) profile->token_context_update_ns += os_monotonic_raw_ns() - t0;

		out_stats->coeff_context_updates++;
		prev_token_was_zero = (token == DCT_0);
	}

	return current_has_coeffs;
}

static int decode_all_coeffs_keyframe(ByteSpan vp8_payload, const Vp8KeyFrameHeader* kf, uint8_t total_partitions,
					  const MbInfo* mbs, uint32_t mb_cols, uint32_t mb_rows, Vp8CoeffStats* out,
					  Vp8DecodedFrame* frame, uint64_t* io_hash, int collect_stats,
					  Vp8EntropyProfile* profile,
					  Vp8MacroblockCoeffVisitor visitor, void* visitor_user,
					  Vp8MacroblockVisitor mb_visitor, void* mb_visitor_user,
					  const uint8_t* bpred_modes) {
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
	BoolDecoderFastState token_bd = bool_decoder_fast_state_load(&d);

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		left_y[0] = left_y[1] = left_y[2] = left_y[3] = 0;
		left_u[0] = left_u[1] = 0;
		left_v[0] = left_v[1] = 0;
		left_y2_flag = 0;

		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb_index = mb_r * mb_cols + mb_c;
			MbInfo info = mbs[mb_index];
			uint8_t has_y2 = (uint8_t)((info.flags & MBINFO_HAS_Y2) != 0);
			uint8_t skip_coeff = (uint8_t)((info.flags & MBINFO_SKIP_COEFF) != 0);
			int mb_has_coeff = 0;
			Vp8MacroblockCoeffs mb_coeffs;
			Vp8MacroblockCoeffs* cb_coeffs = (visitor || mb_visitor) ? &mb_coeffs : NULL;
			if (cb_coeffs) memset(cb_coeffs, 0, sizeof(*cb_coeffs));

			int16_t block[16];
			int16_t* dst = NULL;

			// Y2
			if (has_y2) {
				if (collect_stats) out->blocks_total_y2++;
				uint8_t left_has = left_y2_flag;
				uint8_t above_has = above_y2[mb_c];
				int has = 0;
				if (!skip_coeff) {
					dst = (frame && frame->coeff_y2) ? (frame->coeff_y2 + (size_t)mb_index * 16u) : NULL;
					int16_t* visitor_dst = cb_coeffs ? cb_coeffs->y2 : NULL;
					int16_t* coeff_out = visitor_dst ? visitor_dst : ((io_hash || !dst) ? block : dst);
					if (coeff_out == block) memset(block, 0, sizeof(block));
					if (collect_stats) {
						uint64_t t0 = prof_now_ns(profile);
						has = decode_block_profiled(&d, g_coeff_probs[1], 0, left_has, above_has, coeff_out, out,
						                           profile, mb_index, /*plane=*/1, /*block_index=*/0);
						if (profile) {
							profile->token_plane_ns[1] += os_monotonic_raw_ns() - t0;
							profile->token_block_class[1][entropy_coeffs_classify(coeff_out)]++;
						}
					} else {
						has = decode_block_fast_state(&token_bd, g_coeff_probs[1], 0, left_has, above_has, coeff_out);
					}
					if (io_hash || (dst && coeff_out != dst)) {
						for (int i = 0; i < 16; i++) {
							if (io_hash) *io_hash = fnv1a64_i32(*io_hash, coeff_out[i]);
							if (dst && coeff_out != dst) dst[i] = coeff_out[i];
						}
					}
				} else {
					if (profile) profile->token_block_class[1][COEFFS_ZERO]++;
					if (io_hash) {
						for (int i = 0; i < 16; i++) {
							*io_hash = fnv1a64_i32(*io_hash, 0);
						}
					}
				}
				if (collect_stats && has) out->blocks_nonzero_y2++;
				if (has) mb_has_coeff = 1;
				above_y2[mb_c] = (uint8_t)has;
				left_y2_flag = (uint8_t)has;
			} else {
				(void)dst;
			}

			// Y blocks
			int y_plane = has_y2 ? 0 : 3;
			int first_coeff = has_y2 ? 1 : 0;
			uint8_t* above_y_col = above_y + (size_t)mb_c * 4u;
			uint8_t y_above[4] = {above_y_col[0], above_y_col[1], above_y_col[2], above_y_col[3]};

			for (int rr = 0; rr < 4; rr++) {
				uint8_t left_row_has = left_y[rr];
				for (int cc = 0; cc < 4; cc++) {
					if (collect_stats) out->blocks_total_y++;
					uint8_t left_has = left_row_has;
					uint8_t above_has = y_above[cc];
					int has = 0;
					if (!skip_coeff) {
						size_t blk = (size_t)mb_index * 16u + (size_t)(rr * 4 + cc);
						dst = (frame && frame->coeff_y) ? (frame->coeff_y + blk * 16u) : NULL;
						int16_t* visitor_dst = cb_coeffs ? cb_coeffs->y[rr * 4 + cc] : NULL;
						int16_t* coeff_out = visitor_dst ? visitor_dst : ((io_hash || !dst) ? block : dst);
						if (coeff_out == block) memset(block, 0, sizeof(block));
						if (collect_stats) {
							uint64_t t0 = prof_now_ns(profile);
							has = decode_block_profiled(&d, g_coeff_probs[y_plane], first_coeff, left_has, above_has,
							                           coeff_out, out, profile, mb_index, /*plane=*/0,
							                           /*block_index=*/(uint32_t)(rr * 4 + cc));
							if (profile) {
								profile->token_plane_ns[0] += os_monotonic_raw_ns() - t0;
								profile->token_block_class[0][entropy_coeffs_classify(coeff_out)]++;
							}
						} else {
							has = decode_block_fast_state(&token_bd, g_coeff_probs[y_plane], first_coeff, left_has, above_has, coeff_out);
						}
						if (io_hash || (dst && coeff_out != dst)) {
							for (int i = 0; i < 16; i++) {
								if (io_hash) *io_hash = fnv1a64_i32(*io_hash, coeff_out[i]);
								if (dst && coeff_out != dst) dst[i] = coeff_out[i];
							}
						}
					} else {
						if (profile) profile->token_block_class[0][COEFFS_ZERO]++;
						if (io_hash) {
							for (int i = 0; i < 16; i++) {
								*io_hash = fnv1a64_i32(*io_hash, 0);
							}
						}
					}
					if (collect_stats && has) out->blocks_nonzero_y++;
					if (has) mb_has_coeff = 1;
					left_row_has = (uint8_t)has;
					y_above[cc] = (uint8_t)has;
				}
				left_y[rr] = left_row_has;
			}
			above_y_col[0] = y_above[0];
			above_y_col[1] = y_above[1];
			above_y_col[2] = y_above[2];
			above_y_col[3] = y_above[3];

			// U blocks (2x2)
			uint8_t* above_u_col = above_u + (size_t)mb_c * 2u;
			uint8_t u_above[2] = {above_u_col[0], above_u_col[1]};
			for (int rr = 0; rr < 2; rr++) {
				uint8_t left_row_has = left_u[rr];
				for (int cc = 0; cc < 2; cc++) {
					if (collect_stats) out->blocks_total_u++;
					uint8_t left_has = left_row_has;
					uint8_t above_has = u_above[cc];
					int has = 0;
					if (!skip_coeff) {
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = (frame && frame->coeff_u) ? (frame->coeff_u + blk * 16u) : NULL;
						int16_t* visitor_dst = cb_coeffs ? cb_coeffs->u[rr * 2 + cc] : NULL;
						int16_t* coeff_out = visitor_dst ? visitor_dst : ((io_hash || !dst) ? block : dst);
						if (coeff_out == block) memset(block, 0, sizeof(block));
						if (collect_stats) {
							uint64_t t0 = prof_now_ns(profile);
							has = decode_block_profiled(&d, g_coeff_probs[2], 0, left_has, above_has, coeff_out, out,
							                           profile, mb_index, /*plane=*/2,
							                           /*block_index=*/(uint32_t)(rr * 2 + cc));
							if (profile) {
								profile->token_plane_ns[2] += os_monotonic_raw_ns() - t0;
								profile->token_block_class[2][entropy_coeffs_classify(coeff_out)]++;
							}
						} else {
							has = decode_block_fast_state(&token_bd, g_coeff_probs[2], 0, left_has, above_has, coeff_out);
						}
						if (io_hash || (dst && coeff_out != dst)) {
							for (int i = 0; i < 16; i++) {
								if (io_hash) *io_hash = fnv1a64_i32(*io_hash, coeff_out[i]);
								if (dst && coeff_out != dst) dst[i] = coeff_out[i];
							}
						}
					} else {
						if (profile) profile->token_block_class[2][COEFFS_ZERO]++;
						if (io_hash) {
							for (int i = 0; i < 16; i++) {
								*io_hash = fnv1a64_i32(*io_hash, 0);
							}
						}
					}
					if (collect_stats && has) out->blocks_nonzero_u++;
					if (has) mb_has_coeff = 1;
					left_row_has = (uint8_t)has;
					u_above[cc] = (uint8_t)has;
				}
				left_u[rr] = left_row_has;
			}
			above_u_col[0] = u_above[0];
			above_u_col[1] = u_above[1];

			// V blocks (2x2)
			uint8_t* above_v_col = above_v + (size_t)mb_c * 2u;
			uint8_t v_above[2] = {above_v_col[0], above_v_col[1]};
			for (int rr = 0; rr < 2; rr++) {
				uint8_t left_row_has = left_v[rr];
				for (int cc = 0; cc < 2; cc++) {
					if (collect_stats) out->blocks_total_v++;
					uint8_t left_has = left_row_has;
					uint8_t above_has = v_above[cc];
					int has = 0;
					if (!skip_coeff) {
						size_t blk = (size_t)mb_index * 4u + (size_t)(rr * 2 + cc);
						dst = (frame && frame->coeff_v) ? (frame->coeff_v + blk * 16u) : NULL;
						int16_t* visitor_dst = cb_coeffs ? cb_coeffs->v[rr * 2 + cc] : NULL;
						int16_t* coeff_out = visitor_dst ? visitor_dst : ((io_hash || !dst) ? block : dst);
						if (coeff_out == block) memset(block, 0, sizeof(block));
						if (collect_stats) {
							uint64_t t0 = prof_now_ns(profile);
							has = decode_block_profiled(&d, g_coeff_probs[2], 0, left_has, above_has, coeff_out, out,
							                           profile, mb_index, /*plane=*/3,
							                           /*block_index=*/(uint32_t)(rr * 2 + cc));
							if (profile) {
								profile->token_plane_ns[3] += os_monotonic_raw_ns() - t0;
								profile->token_block_class[3][entropy_coeffs_classify(coeff_out)]++;
							}
						} else {
							has = decode_block_fast_state(&token_bd, g_coeff_probs[2], 0, left_has, above_has, coeff_out);
						}
						if (io_hash || (dst && coeff_out != dst)) {
							for (int i = 0; i < 16; i++) {
								if (io_hash) *io_hash = fnv1a64_i32(*io_hash, coeff_out[i]);
								if (dst && coeff_out != dst) dst[i] = coeff_out[i];
							}
						}
					} else {
						if (profile) profile->token_block_class[3][COEFFS_ZERO]++;
						if (io_hash) {
							for (int i = 0; i < 16; i++) {
								*io_hash = fnv1a64_i32(*io_hash, 0);
							}
						}
					}
					if (collect_stats && has) out->blocks_nonzero_v++;
					if (has) mb_has_coeff = 1;
					left_row_has = (uint8_t)has;
					v_above[cc] = (uint8_t)has;
				}
				left_v[rr] = left_row_has;
			}
			above_v_col[0] = v_above[0];
			above_v_col[1] = v_above[1];

			if (frame && frame->has_coeff) frame->has_coeff[mb_index] = (uint8_t)(mb_has_coeff != 0);
			if (mb_visitor) {
				Vp8MacroblockSyntax syntax = {
					.segment_id = info.segment_id,
					.skip_coeff = skip_coeff,
					.has_coeff = (uint8_t)(mb_has_coeff != 0),
					.has_y2 = has_y2,
					.ymode = info.ymode,
					.uv_mode = info.uv_mode,
					.bmode = (info.bmode_offset != MBINFO_NO_BMODE_OFFSET && bpred_modes) ?
					         (bpred_modes + info.bmode_offset) :
					         NULL,
				};
				if (mb_visitor(mb_visitor_user, mb_index, &syntax, cb_coeffs) != 0) {
					free(above_y);
					free(above_u);
					free(above_v);
					free(above_y2);
					if (errno == 0) errno = EINVAL;
					return -1;
				}
			}
			if (visitor && visitor(visitor_user, mb_index, cb_coeffs) != 0) {
				free(above_y);
				free(above_u);
				free(above_v);
				free(above_y2);
				if (errno == 0) errno = EINVAL;
				return -1;
			}
		}
	}

	if (!collect_stats) bool_decoder_fast_state_store(&d, &token_bd);
	out->token_part_bytes_used = (uint32_t)bool_decoder_bytes_used(&d);
	if (out->token_part_bytes_used > out->token_part_size_bytes) {
		free(above_y);
		free(above_u);
		free(above_v);
		free(above_y2);
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

static int vp8_decode_decoded_frame_internal(ByteSpan vp8_payload,
                                             Vp8DecodedFrame* out,
                                             int collect_stats,
                                             int store_syntax,
                                             int store_coeffs,
                                             Vp8EntropyProfile* profile,
                                             Vp8MacroblockCoeffVisitor visitor,
                                             void* visitor_user,
                                             Vp8MacroblockVisitor mb_visitor,
                                             void* mb_visitor_user) {
	if (!out) return -1;
	*out = (Vp8DecodedFrame){0};
	entropy_profile_zero(profile);

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
	out->stats.token_overread_mb_index = 0xFFFFFFFFu;
	out->stats.token_overread_plane = 0xFFFFFFFFu;
	out->stats.token_overread_block_index = 0xFFFFFFFFu;
	out->stats.token_overread_coeff_i = 0xFFFFFFFFu;
	out->stats.token_overread_stage = 0xFFFFFFFFu;

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

	if (store_syntax) {
		out->segment_id = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
		out->skip_coeff = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
		out->has_coeff = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
		out->ymode = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
		out->uv_mode = (uint8_t*)xcalloc_array(mb_total, sizeof(uint8_t));
		out->bmode = (uint8_t*)xcalloc_array((size_t)mb_total * 16u, sizeof(uint8_t));
		if (!out->segment_id || !out->skip_coeff || !out->has_coeff || !out->ymode || !out->uv_mode || !out->bmode) {
			vp8_decoded_frame_free(out);
			errno = ENOMEM;
			return -1;
		}
	}
	if (store_coeffs) {
		out->coeff_y2 = (int16_t*)xcalloc_array((size_t)mb_total * 16u, sizeof(int16_t));
		out->coeff_y = (int16_t*)xcalloc_array((size_t)mb_total * 16u * 16u, sizeof(int16_t));
		out->coeff_u = (int16_t*)xcalloc_array((size_t)mb_total * 4u * 16u, sizeof(int16_t));
		out->coeff_v = (int16_t*)xcalloc_array((size_t)mb_total * 4u * 16u, sizeof(int16_t));
		if (!out->coeff_y2 || !out->coeff_y || !out->coeff_u || !out->coeff_v) {
			vp8_decoded_frame_free(out);
			errno = ENOMEM;
			return -1;
		}
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
	uint64_t part0_header_start = prof_now_ns(profile);

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
	init_coeff_probs_defaults();
	update_coeff_probs(&d);

	// mb_no_skip_coeff + prob_skip_false
	int mb_no_skip_coeff = bool_decode_bool(&d, 128);
	uint8_t prob_skip_false = 0;
	if (mb_no_skip_coeff) {
		prob_skip_false = (uint8_t)bool_decode_literal(&d, 8);
	}

	// Token partition size table is ignored for now; our test corpus has Total partitions: 1.
	prof_add_ns(profile ? &profile->part0_header_ns : NULL, part0_header_start);

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
	BPredModeStore bpred_store = {0};
	BoolDecoderFastState mb_bd = bool_decoder_fast_state_load(&d);

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		intra_bmode left_bmodes[4] = {B_DC_PRED, B_DC_PRED, B_DC_PRED, B_DC_PRED};
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint64_t mb_syntax_start = prof_now_ns(profile);
			uint32_t mb_index = mb_r * mb_cols + mb_c;
			uint8_t seg_id = 0;
			mbs[mb_index].bmode_offset = MBINFO_NO_BMODE_OFFSET;
			if (segmentation_enabled && update_mb_segmentation_map) {
				uint64_t t0 = prof_now_ns(profile);
				seg_id = read_mb_segment_fast(&mb_bd, mb_segment_tree_probs);
				prof_add_ns(profile ? &profile->part0_segment_read_ns : NULL, t0);
			}
			mbs[mb_index].segment_id = seg_id;
			if (out->segment_id) out->segment_id[mb_index] = seg_id;

			uint8_t skip_coeff = 0;
			if (mb_no_skip_coeff) {
				uint64_t t0 = prof_now_ns(profile);
				skip_coeff = (uint8_t)bool_fast_decode_bool(&mb_bd, prob_skip_false);
				prof_add_ns(profile ? &profile->part0_skip_read_ns : NULL, t0);
			}
			if (skip_coeff) mbs[mb_index].flags |= MBINFO_SKIP_COEFF;
			if (out->skip_coeff) out->skip_coeff[mb_index] = skip_coeff;
			if (collect_stats && skip_coeff) out->stats.mb_skip_coeff++;

			uint64_t ymode_t0 = prof_now_ns(profile);
			intra_mbmode ymode = read_kf_ymode_fast(&mb_bd);
			prof_add_ns(profile ? &profile->part0_ymode_read_ns : NULL, ymode_t0);
			mbs[mb_index].ymode = (uint8_t)ymode;
			if (out->ymode) out->ymode[mb_index] = (uint8_t)ymode;
			if (collect_stats && (unsigned)ymode < 5u) out->stats.ymode_counts[(unsigned)ymode]++;
			if (ymode == B_PRED) {
				if (collect_stats) out->stats.mb_b_pred++;
				intra_bmode local[4][4];
				for (int rr = 0; rr < 4; rr++)
					for (int cc = 0; cc < 4; cc++) local[rr][cc] = B_DC_PRED;
				uint64_t bmode_t0 = prof_now_ns(profile);
				for (int rr = 0; rr < 4; rr++) {
					for (int cc = 0; cc < 4; cc++) {
						intra_bmode A = (rr == 0) ? above_bmodes[mb_c * 4 + cc] : local[rr - 1][cc];
						intra_bmode L = (cc == 0) ? left_bmodes[rr] : local[rr][cc - 1];
						const uint8_t* probs = kf_bmode_prob[A][L];
						local[rr][cc] = read_kf_bmode_fast(&mb_bd, probs);
						if (out->bmode) out->bmode[(size_t)mb_index * 16u + (size_t)(rr * 4 + cc)] = (uint8_t)local[rr][cc];
						if (collect_stats && (unsigned)local[rr][cc] < 10u) out->stats.bmode_counts[(unsigned)local[rr][cc]]++;
					}
				}
				prof_add_ns(profile ? &profile->part0_bmode_read_ns : NULL, bmode_t0);
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4 + cc] = local[3][cc];
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = local[rr][3];
				if (mb_visitor && bpred_mode_store_append(&bpred_store, local, &mbs[mb_index].bmode_offset) != 0) {
					bpred_mode_store_free(&bpred_store);
					free(above_bmodes);
					free(mbs);
					vp8_decoded_frame_free(out);
					return -1;
				}
			} else {
				mbs[mb_index].flags |= MBINFO_HAS_Y2;
				intra_bmode derived = mbmode_to_bmode(ymode);
				for (int cc = 0; cc < 4; cc++) above_bmodes[mb_c * 4 + cc] = derived;
				for (int rr = 0; rr < 4; rr++) left_bmodes[rr] = derived;
				for (int rr = 0; rr < 4; rr++)
					for (int cc = 0; cc < 4; cc++)
						if (out->bmode) out->bmode[(size_t)mb_index * 16u + (size_t)(rr * 4 + cc)] = (uint8_t)derived;
			}

			uint64_t uv_t0 = prof_now_ns(profile);
			unsigned uv_mode = read_kf_uv_mode_fast(&mb_bd);
			prof_add_ns(profile ? &profile->part0_uvmode_read_ns : NULL, uv_t0);
			mbs[mb_index].uv_mode = (uint8_t)uv_mode;
			if (out->uv_mode) out->uv_mode[mb_index] = (uint8_t)uv_mode;
			if (collect_stats && uv_mode < 4u) out->stats.uv_mode_counts[uv_mode]++;
			prof_add_ns(profile ? &profile->part0_mb_syntax_ns : NULL, mb_syntax_start);
		}
	}

	bool_decoder_fast_state_store(&d, &mb_bd);
	out->stats.part0_bytes_used = (uint32_t)bool_decoder_bytes_used(&d);
	if (out->stats.part0_bytes_used > out->stats.part0_size_bytes) {
		errno = EINVAL;
		bpred_mode_store_free(&bpred_store);
		free(above_bmodes);
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	out->stats.part0_overread = (uint8_t)(bool_decoder_overread(&d) != 0);
	out->stats.part0_overread_bytes = bool_decoder_overread_bytes(&d);

	// RFC-aligned internal consistency checks.
	if (collect_stats) {
		uint32_t ysum = 0;
		for (int i = 0; i < 5; i++) ysum += out->stats.ymode_counts[i];
		if (ysum != mb_total) {
			errno = EINVAL;
			bpred_mode_store_free(&bpred_store);
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
		uint32_t uvsum = 0;
		for (int i = 0; i < 4; i++) uvsum += out->stats.uv_mode_counts[i];
		if (uvsum != mb_total) {
			errno = EINVAL;
			bpred_mode_store_free(&bpred_store);
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
		uint32_t bsum = 0;
		for (int i = 0; i < 10; i++) bsum += out->stats.bmode_counts[i];
		if (bsum != out->stats.mb_b_pred * 16u) {
			errno = EINVAL;
			bpred_mode_store_free(&bpred_store);
			free(above_bmodes);
			free(mbs);
			vp8_decoded_frame_free(out);
			return -1;
		}
	}

	uint64_t h = collect_stats ? fnv1a64_init() : 0;
	uint64_t token_decode_start = prof_now_ns(profile);
	if (decode_all_coeffs_keyframe(vp8_payload, &kf, total_partitions, mbs, mb_cols, mb_rows, &out->stats,
	                               (store_syntax || store_coeffs) ? out : NULL, collect_stats ? &h : NULL, collect_stats, profile,
	                               visitor, visitor_user, mb_visitor, mb_visitor_user, bpred_store.data) != 0) {
		bpred_mode_store_free(&bpred_store);
		free(above_bmodes);
		free(mbs);
		vp8_decoded_frame_free(out);
		return -1;
	}
	prof_add_ns(profile ? &profile->token_decode_ns : NULL, token_decode_start);
	bpred_mode_store_free(&bpred_store);
	free(above_bmodes);

	if (collect_stats) {
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
	}
	free(mbs);
	return 0;
}

int vp8_decode_decoded_frame(ByteSpan vp8_payload, Vp8DecodedFrame* out) {
	return vp8_decode_decoded_frame_internal(vp8_payload, out, /*collect_stats=*/0, /*store_syntax=*/1, /*store_coeffs=*/1,
	                                         NULL, NULL, NULL, NULL, NULL);
}

int vp8_decode_decoded_frame_profiled(ByteSpan vp8_payload, Vp8DecodedFrame* out, Vp8EntropyProfile* profile) {
	return vp8_decode_decoded_frame_internal(vp8_payload, out, /*collect_stats=*/1, /*store_syntax=*/1, /*store_coeffs=*/1,
	                                         profile, NULL, NULL, NULL, NULL);
}

int vp8_decode_decoded_frame_visit_coeffs(ByteSpan vp8_payload, Vp8DecodedFrame* out,
                                          Vp8MacroblockCoeffVisitor visitor, void* user) {
	if (!visitor) {
		errno = EINVAL;
		return -1;
	}
	return vp8_decode_decoded_frame_internal(vp8_payload, out, /*collect_stats=*/0, /*store_syntax=*/1, /*store_coeffs=*/0,
	                                         NULL, visitor, user, NULL, NULL);
}

int vp8_decode_decoded_frame_visit_macroblocks(ByteSpan vp8_payload, Vp8DecodedFrame* out,
                                               Vp8MacroblockVisitor visitor, void* user) {
	if (!visitor) {
		errno = EINVAL;
		return -1;
	}
	return vp8_decode_decoded_frame_internal(vp8_payload, out, /*collect_stats=*/0, /*store_syntax=*/0, /*store_coeffs=*/0,
	                                         NULL, NULL, NULL, visitor, user);
}

int vp8_decode_coeff_stats(ByteSpan vp8_payload, Vp8CoeffStats* out) {
	if (!out) return -1;
	Vp8DecodedFrame f;
	if (vp8_decode_decoded_frame_internal(vp8_payload, &f, /*collect_stats=*/1, /*store_syntax=*/0, /*store_coeffs=*/0,
	                                      NULL, NULL, NULL, NULL, NULL) != 0)
		return -1;
	*out = f.stats;
	vp8_decoded_frame_free(&f);
	return 0;
}
