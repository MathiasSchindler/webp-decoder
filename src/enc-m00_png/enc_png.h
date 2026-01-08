#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EncPngImage {
	uint32_t width;
	uint32_t height;
	uint8_t channels; /* 3 = RGB, 4 = RGBA */
	uint8_t* data;    /* tightly packed, row-major */
} EncPngImage;

/*
Reads a PNG file into memory.

Supported subset (for Milestone 0):
- 8-bit per channel
- color type 2 (RGB) or 6 (RGBA)
- non-interlaced
- standard DEFLATE/zlib-compressed IDAT

Returns 0 on success, -1 on failure.
*/
int enc_png_read_file(const char* path, EncPngImage* out_img);

void enc_png_free(EncPngImage* img);

#ifdef __cplusplus
}
#endif
