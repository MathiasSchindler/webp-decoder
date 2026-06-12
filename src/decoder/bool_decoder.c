#include "bool_decoder.h"

#include <errno.h>

static void refill(BoolDecoder* d) {
	while (d->count >= 0) {
		if (d->buf < d->end) {
			d->value |= (uint32_t)(*d->buf++) << d->count;
		} else {
			d->overread = 1;
			d->overread_bytes++;
		}
		d->count -= 8;
	}
}

int bool_decoder_init(BoolDecoder* d, ByteSpan data) {
	if (!d || (!data.data && data.size != 0)) {
		errno = EINVAL;
		return -1;
	}
	d->start = data.data;
	d->buf = data.data;
	d->end = data.data + data.size;
	d->range = 255;
	d->value = 0;
	if (data.size >= 1) {
		d->value |= (uint32_t)d->buf[0] << 8;
		d->buf += 1;
	}
	if (data.size >= 2) {
		d->value |= (uint32_t)d->buf[0];
		d->buf += 1;
	}
	d->count = -8;
	d->overread = 0;
	d->overread_bytes = 0;
	return 0;
}

int bool_decode_bool(BoolDecoder* d, uint8_t prob) {
	// See RFC 6386 Section 7.
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

	int shift = 0;
	while (range < 128) {
		range <<= 1;
		shift++;
	}

	d->range = range;
	d->value = value << shift;
	d->count += shift;
	refill(d);
	return bit;
}

uint32_t bool_decode_literal(BoolDecoder* d, int bits) {
	uint32_t v = 0;
	for (int i = bits - 1; i >= 0; i--) {
		v |= (uint32_t)bool_decode_bool(d, 128) << i;
	}
	return v;
}

int32_t bool_decode_sint(BoolDecoder* d, int bits) {
	uint32_t mag = bool_decode_literal(d, bits);
	if (mag == 0) return 0;
	int sign = bool_decode_bool(d, 128);
	return sign ? -(int32_t)mag : (int32_t)mag;
}

size_t bool_decoder_bytes_used(const BoolDecoder* d) {
	if (!d || !d->start) return 0;
	if (d->buf < d->start) return 0;
	return (size_t)(d->buf - d->start);
}

int bool_decoder_overread(const BoolDecoder* d) {
	return d && d->overread != 0;
}

uint32_t bool_decoder_overread_bytes(const BoolDecoder* d) {
	if (!d) return 0;
	return d->overread_bytes;
}
