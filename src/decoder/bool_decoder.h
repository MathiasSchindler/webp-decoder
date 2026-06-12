#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../common/os.h"

typedef struct {
	const uint8_t* start;
	const uint8_t* buf;
	const uint8_t* end;
	uint32_t value;
	uint8_t range;
	int count;
	uint8_t overread;
	uint32_t overread_bytes;
} BoolDecoder;

#if defined(__GNUC__) || defined(__clang__)
#define BOOL_DECODER_LIKELY(x) __builtin_expect(!!(x), 1)
#define BOOL_DECODER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BOOL_DECODER_LIKELY(x) (x)
#define BOOL_DECODER_UNLIKELY(x) (x)
#endif

static inline uint8_t bool_decoder_norm_shift(uint8_t range) {
	static const uint8_t shift[256] = {
		0, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	return shift[range];
}

static inline void bool_decoder_refill_inline(BoolDecoder* d) {
	if (BOOL_DECODER_LIKELY(d->buf < d->end)) {
		d->value |= (uint32_t)(*d->buf++) << d->count;
	} else {
		d->overread = 1;
		d->overread_bytes++;
	}
	d->count -= 8;
}

static inline int bool_decode_bool_inline(BoolDecoder* d, uint8_t prob) {
	uint8_t range = d->range;
	uint32_t value = d->value;
	uint32_t split = 1u + (((uint32_t)(range - 1u) * (uint32_t)prob) >> 8);
	uint32_t bigsplit = split << 8;

	int bit;
	if (value >= bigsplit) {
		range = (uint8_t)(range - split);
		value -= bigsplit;
		bit = 1;
	} else {
		range = (uint8_t)split;
		bit = 0;
	}

	uint8_t shift = bool_decoder_norm_shift(range);
	d->range = (uint8_t)(range << shift);
	d->value = value << shift;
	d->count += shift;
	if (BOOL_DECODER_UNLIKELY(d->count >= 0)) bool_decoder_refill_inline(d);
	return bit;
}

static inline int bool_decode_bit_inline(BoolDecoder* d) {
	uint8_t range = d->range;
	uint32_t value = d->value;
	uint32_t split = ((uint32_t)range + 1u) >> 1;
	uint32_t bigsplit = split << 8;

	int bit;
	if (value >= bigsplit) {
		range = (uint8_t)(range - split);
		value -= bigsplit;
		bit = 1;
	} else {
		range = (uint8_t)split;
		bit = 0;
	}

	uint8_t shift = bool_decoder_norm_shift(range);
	d->range = (uint8_t)(range << shift);
	d->value = value << shift;
	d->count += shift;
	if (BOOL_DECODER_UNLIKELY(d->count >= 0)) bool_decoder_refill_inline(d);
	return bit;
}

static inline uint32_t bool_decode_literal_inline(BoolDecoder* d, int bits) {
	uint32_t v = 0;
	for (int i = bits - 1; i >= 0; i--) {
		v |= (uint32_t)bool_decode_bit_inline(d) << i;
	}
	return v;
}

static inline int32_t bool_decode_sint_inline(BoolDecoder* d, int bits) {
	uint32_t mag = bool_decode_literal_inline(d, bits);
	if (mag == 0) return 0;
	int sign = bool_decode_bit_inline(d);
	return sign ? -(int32_t)mag : (int32_t)mag;
}

// Initialize a VP8 boolean decoder on the given buffer.
// Returns 0 on success.
int bool_decoder_init(BoolDecoder* d, ByteSpan data);

// Decode a single boolean with the given probability (0..255).
int bool_decode_bool(BoolDecoder* d, uint8_t prob);

// Decode an n-bit literal using prob=128.
uint32_t bool_decode_literal(BoolDecoder* d, int bits);

// Decode a signed value as (magnitude literal bits) + sign bit.
int32_t bool_decode_sint(BoolDecoder* d, int bits);

// Returns the number of bytes consumed from the input partition so far.
size_t bool_decoder_bytes_used(const BoolDecoder* d);

// Returns non-zero if decoding attempted to refill past the end of the partition.
int bool_decoder_overread(const BoolDecoder* d);

// Returns number of bytes that decoding attempted to read beyond the end.
uint32_t bool_decoder_overread_bytes(const BoolDecoder* d);
