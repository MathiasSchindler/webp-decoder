#include "enc_bool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int ensure_cap(EncBoolEncoder* e, size_t add) {
	if (e->error) return -1;
	if (e->size + add <= e->cap) return 0;
	size_t new_cap = e->cap ? e->cap : 256;
	while (new_cap < e->size + add) new_cap *= 2;
	uint8_t* grown = (uint8_t*)realloc(e->buf, new_cap);
	if (!grown) {
		e->error = 1;
		errno = ENOMEM;
		return -1;
	}
	e->buf = grown;
	e->cap = new_cap;
	return 0;
}

static void add_one_to_output(EncBoolEncoder* e) {
	// RFC 6386 Section 7.3: propagate carry into already-written output.
	if (e->error) return;
	if (e->size == 0) {
		e->error = 1;
		return;
	}
	size_t i = e->size;
	while (i > 0) {
		i--;
		if (e->buf[i] == 255) {
			e->buf[i] = 0;
			continue;
		}
		e->buf[i]++;
		return;
	}
	// Should be unreachable per arithmetic guarantees; treat as error.
	e->error = 1;
}

static void put_u8(EncBoolEncoder* e, uint8_t v) {
	if (ensure_cap(e, 1) != 0) return;
	e->buf[e->size++] = v;
}

void enc_bool_init(EncBoolEncoder* e) {
	if (!e) return;
	memset(e, 0, sizeof(*e));
	e->range = 255;
	e->bottom = 0;
	e->bit_count = 24;
}

void enc_bool_free(EncBoolEncoder* e) {
	if (!e) return;
	free(e->buf);
	memset(e, 0, sizeof(*e));
}

void enc_bool_put(EncBoolEncoder* e, uint8_t prob, int bit) {
	if (!e || e->error) return;
	// RFC 6386 Section 7.3 encoder.
	uint32_t split = 1u + (((e->range - 1u) * (uint32_t)prob) >> 8);
	if (bit) {
		e->bottom += split;
		e->range -= split;
	} else {
		e->range = split;
	}

	while (e->range < 128) {
		e->range <<= 1;

		if (e->bottom & (1u << 31)) {
			add_one_to_output(e);
			if (e->error) return;
		}

		e->bottom <<= 1;

		e->bit_count--;
		if (e->bit_count == 0) {
			put_u8(e, (uint8_t)(e->bottom >> 24));
			e->bottom &= (1u << 24) - 1u;
			e->bit_count = 8;
			if (e->error) return;
		}
	}
}

void enc_bool_put_literal(EncBoolEncoder* e, uint32_t value, int bits) {
	for (int i = bits - 1; i >= 0; i--) {
		enc_bool_put(e, 128, (int)((value >> i) & 1u));
		if (e && e->error) return;
	}
}

void enc_bool_finish(EncBoolEncoder* e) {
	if (!e || e->error) return;
	int c = e->bit_count;
	uint32_t v = e->bottom;

	// Propagate carry if needed (rare).
	if (v & (1u << (32 - c))) {
		add_one_to_output(e);
		if (e->error) return;
	}

	v <<= (uint32_t)(c & 7);
	c >>= 3;
	while (--c >= 0) v <<= 8;

	for (int i = 0; i < 4; i++) {
		put_u8(e, (uint8_t)(v >> 24));
		v <<= 8;
		if (e->error) return;
	}
}

const uint8_t* enc_bool_data(const EncBoolEncoder* e) {
	return e ? e->buf : NULL;
}

size_t enc_bool_size(const EncBoolEncoder* e) {
	return e ? e->size : 0;
}

int enc_bool_error(const EncBoolEncoder* e) {
	return e ? e->error : 1;
}
