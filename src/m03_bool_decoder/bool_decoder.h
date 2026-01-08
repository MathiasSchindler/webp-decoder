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
