#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../common/os.h"

typedef struct {
	uint32_t riff_size;   // As stored in RIFF header (bytes from offset 8)
	size_t actual_size;   // Actual mapped file size

	// For milestone 1 we only support simple lossy: exactly one VP8 chunk.
	size_t vp8_chunk_offset; // Offset of VP8 payload (not header)
	uint32_t vp8_chunk_size; // Size of VP8 payload
} WebPContainer;

// Parses a WebP container (RFC 9649) with simple lossy layout (VP8 chunk).
// Returns 0 on success.
int webp_parse_simple_lossy(ByteSpan file, WebPContainer* out);
