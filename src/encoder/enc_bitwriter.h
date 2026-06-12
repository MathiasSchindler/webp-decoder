#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EncBitWriter {
	uint8_t* buf;
	size_t size;
	size_t cap;
	uint32_t bitbuf;
	int bitcount; /* number of bits currently in bitbuf */
	int error;
} EncBitWriter;

void enc_bw_init(EncBitWriter* w);
void enc_bw_free(EncBitWriter* w);

/* Writes n bits, least-significant-bit first (common for packed bitstreams). */
void enc_bw_put_bits(EncBitWriter* w, uint32_t bits, int n);

void enc_bw_put_u8(EncBitWriter* w, uint8_t v);
void enc_bw_put_u16le(EncBitWriter* w, uint16_t v);
void enc_bw_put_u24le(EncBitWriter* w, uint32_t v);
void enc_bw_put_u32le(EncBitWriter* w, uint32_t v);

/* Flush to next byte boundary (pads with zero bits). */
void enc_bw_flush_to_byte(EncBitWriter* w);

const uint8_t* enc_bw_data(const EncBitWriter* w);
size_t enc_bw_size(const EncBitWriter* w);
int enc_bw_error(const EncBitWriter* w);

#ifdef __cplusplus
}
#endif
