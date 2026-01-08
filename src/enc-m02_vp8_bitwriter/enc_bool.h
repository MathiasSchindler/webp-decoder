#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EncBoolEncoder {
	uint8_t* buf;
	size_t size;
	size_t cap;
	uint32_t range;     /* 128 <= range <= 255 */
	uint32_t bottom;    /* interval bottom (top bits may carry) */
	int bit_count;      /* shifts until a full output byte is available */
	int error;
} EncBoolEncoder;

void enc_bool_init(EncBoolEncoder* e);
void enc_bool_free(EncBoolEncoder* e);

/* Encodes a single boolean with probability prob/256 of being 0. */
void enc_bool_put(EncBoolEncoder* e, uint8_t prob, int bit);

/* Writes bits high-to-low order using prob=128 (matches decoder literal). */
void enc_bool_put_literal(EncBoolEncoder* e, uint32_t value, int bits);

/* Finalize and append trailing bytes. Call exactly once per partition. */
void enc_bool_finish(EncBoolEncoder* e);

/* Access output after finish. */
const uint8_t* enc_bool_data(const EncBoolEncoder* e);
size_t enc_bool_size(const EncBoolEncoder* e);
int enc_bool_error(const EncBoolEncoder* e);

#ifdef __cplusplus
}
#endif
