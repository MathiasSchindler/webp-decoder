#include "enc_bitwriter.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int ensure_cap(EncBitWriter* w, size_t add) {
	if (w->error) return -1;
	if (w->size + add <= w->cap) return 0;
	size_t new_cap = w->cap ? w->cap : 256;
	while (new_cap < w->size + add) new_cap *= 2;
	uint8_t* grown = (uint8_t*)realloc(w->buf, new_cap);
	if (!grown) {
		w->error = 1;
		errno = ENOMEM;
		return -1;
	}
	w->buf = grown;
	w->cap = new_cap;
	return 0;
}

static void put_byte(EncBitWriter* w, uint8_t v) {
	if (ensure_cap(w, 1) != 0) return;
	w->buf[w->size++] = v;
}

void enc_bw_init(EncBitWriter* w) {
	if (!w) return;
	memset(w, 0, sizeof(*w));
}

void enc_bw_free(EncBitWriter* w) {
	if (!w) return;
	free(w->buf);
	memset(w, 0, sizeof(*w));
}

void enc_bw_put_bits(EncBitWriter* w, uint32_t bits, int n) {
	if (!w || w->error) return;
	if (n < 0 || n > 24) {
		w->error = 1;
		return;
	}
	uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((n == 0) ? 0u : ((1u << n) - 1u));
	bits &= mask;

	w->bitbuf |= bits << w->bitcount;
	w->bitcount += n;
	while (w->bitcount >= 8) {
		put_byte(w, (uint8_t)(w->bitbuf & 0xFFu));
		w->bitbuf >>= 8;
		w->bitcount -= 8;
		if (w->error) return;
	}
}

void enc_bw_put_u8(EncBitWriter* w, uint8_t v) {
	enc_bw_flush_to_byte(w);
	put_byte(w, v);
}

void enc_bw_put_u16le(EncBitWriter* w, uint16_t v) {
	enc_bw_flush_to_byte(w);
	put_byte(w, (uint8_t)(v & 0xFFu));
	put_byte(w, (uint8_t)((v >> 8) & 0xFFu));
}

void enc_bw_put_u24le(EncBitWriter* w, uint32_t v) {
	enc_bw_flush_to_byte(w);
	put_byte(w, (uint8_t)(v & 0xFFu));
	put_byte(w, (uint8_t)((v >> 8) & 0xFFu));
	put_byte(w, (uint8_t)((v >> 16) & 0xFFu));
}

void enc_bw_put_u32le(EncBitWriter* w, uint32_t v) {
	enc_bw_flush_to_byte(w);
	put_byte(w, (uint8_t)(v & 0xFFu));
	put_byte(w, (uint8_t)((v >> 8) & 0xFFu));
	put_byte(w, (uint8_t)((v >> 16) & 0xFFu));
	put_byte(w, (uint8_t)((v >> 24) & 0xFFu));
}

void enc_bw_flush_to_byte(EncBitWriter* w) {
	if (!w || w->error) return;
	if (w->bitcount > 0) {
		put_byte(w, (uint8_t)(w->bitbuf & 0xFFu));
		w->bitbuf = 0;
		w->bitcount = 0;
	}
}

const uint8_t* enc_bw_data(const EncBitWriter* w) {
	return w ? w->buf : NULL;
}

size_t enc_bw_size(const EncBitWriter* w) {
	return w ? w->size : 0;
}

int enc_bw_error(const EncBitWriter* w) {
	return w ? w->error : 1;
}
