#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns ceil(v/16)*16, or 0 on overflow. */
uint32_t enc_pad16_u32(uint32_t v);

/* Computes macroblock grid for VP8 keyframes: ceil(width/16), ceil(height/16). */
int enc_vp8_mb_grid(uint32_t width, uint32_t height, uint32_t* out_mb_cols, uint32_t* out_mb_rows);

#ifdef __cplusplus
}
#endif
