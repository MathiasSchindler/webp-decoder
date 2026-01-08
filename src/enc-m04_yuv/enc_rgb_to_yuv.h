#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EncYuv420Image {
	uint32_t width;
	uint32_t height;
	uint32_t y_stride;  /* bytes per Y row (currently == width) */
	uint32_t uv_stride; /* bytes per U/V row (currently == ceil(width/2)) */
	uint8_t* y;
	uint8_t* u;
	uint8_t* v;
} EncYuv420Image;

/*
Converts interleaved RGB/RGBA pixels to VP8-style limited-range YUV420,
mirroring libwebp's scalar conversion:

- BT.601-style coefficients
- Y offset 16, U/V offset 128
- Gamma-compressed chroma averaging (kGamma=0.80)
- 4:2:0 downsampling with edge replication for odd sizes

Arguments:
- rgb points to the top-left pixel.
- rgb_stride is bytes per row.
- rgb_step is bytes per pixel (3 for RGB, 4 for RGBA; alpha ignored).

On success, allocates planes via malloc; caller must free via enc_yuv420_free().
Returns 0 on success, -1 on failure (errno set).
*/
int enc_yuv420_from_rgb_libwebp(const uint8_t* rgb,
                               uint32_t width,
                               uint32_t height,
                               uint32_t rgb_stride,
                               uint32_t rgb_step,
                               EncYuv420Image* out);

void enc_yuv420_free(EncYuv420Image* img);

#ifdef __cplusplus
}
#endif
