#include "bool_decoder.h"

#include <errno.h>

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
	return bool_decode_bool_inline(d, prob);
}

uint32_t bool_decode_literal(BoolDecoder* d, int bits) {
	return bool_decode_literal_inline(d, bits);
}

int32_t bool_decode_sint(BoolDecoder* d, int bits) {
	return bool_decode_sint_inline(d, bits);
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
