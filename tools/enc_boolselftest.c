#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/enc-m02_vp8_bitwriter/enc_bool.h"
#include "../src/m03_bool_decoder/bool_decoder.h"

static uint32_t xorshift32(uint32_t* s) {
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	// Deterministic pseudo-random test vectors.
	const int N = 20000;
	uint8_t* probs = (uint8_t*)malloc((size_t)N);
	uint8_t* bits = (uint8_t*)malloc((size_t)N);
	if (!probs || !bits) {
		fprintf(stderr, "alloc failed\n");
		return 1;
	}

	uint32_t seed = 0x12345678u;
	for (int i = 0; i < N; i++) {
		uint32_t r = xorshift32(&seed);
		uint8_t p = (uint8_t)(1u + (r & 0xFEu)); // [1..255] odd/even mix, exclude 0
		probs[i] = p;
		bits[i] = (uint8_t)((r >> 8) & 1u);
	}

	EncBoolEncoder enc;
	enc_bool_init(&enc);

	// Also exercise literal helper.
	enc_bool_put_literal(&enc, 0xA5u, 8);
	for (int i = 0; i < N; i++) enc_bool_put(&enc, probs[i], bits[i]);
	enc_bool_finish(&enc);

	if (enc_bool_error(&enc)) {
		fprintf(stderr, "encoder error\n");
		enc_bool_free(&enc);
		free(probs);
		free(bits);
		return 1;
	}

	ByteSpan bs;
	bs.data = enc_bool_data(&enc);
	bs.size = enc_bool_size(&enc);

	BoolDecoder dec;
	if (bool_decoder_init(&dec, bs) != 0) {
		fprintf(stderr, "bool_decoder_init failed\n");
		enc_bool_free(&enc);
		free(probs);
		free(bits);
		return 1;
	}

	uint32_t lit = bool_decode_literal(&dec, 8);
	if (lit != 0xA5u) {
		fprintf(stderr, "literal mismatch: got=%u\n", lit);
		enc_bool_free(&enc);
		free(probs);
		free(bits);
		return 1;
	}

	for (int i = 0; i < N; i++) {
		int b = bool_decode_bool(&dec, probs[i]);
		if (b != (int)bits[i]) {
			fprintf(stderr, "bit mismatch at %d: got=%d want=%d\n", i, b, (int)bits[i]);
			enc_bool_free(&enc);
			free(probs);
			free(bits);
			return 1;
		}
	}

	// Sanity: should not overread; stream has padding bytes, so this is strict.
	if (bool_decoder_overread(&dec)) {
		fprintf(stderr, "decoder overread (%u bytes)\n", bool_decoder_overread_bytes(&dec));
		enc_bool_free(&enc);
		free(probs);
		free(bits);
		return 1;
	}

	enc_bool_free(&enc);
	free(probs);
	free(bits);
	return 0;
}
