#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
Writes a minimal RIFF/WebP container with a single `VP8 ` chunk.

Layout:
- RIFF header
- WEBP signature
- VP8 chunk header
- VP8 payload
- 0 pad byte if payload size is odd

Returns 0 on success, -1 on failure.
*/
int enc_webp_write_vp8_file(const char* out_path, const uint8_t* vp8_payload, size_t vp8_size);

#ifdef __cplusplus
}
#endif
