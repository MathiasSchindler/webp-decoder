#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
VP8 forward transform (4x4), mirroring libwebp's scalar FTransform_C.

- src/ref are pointers to the top-left of a 4x4 block.
- src_stride/ref_stride are bytes per row.
- out receives 16 coefficients in the same layout as libwebp (row-major 4x4).
*/
void enc_vp8_ftransform4x4(const uint8_t* src,
                           int src_stride,
                           const uint8_t* ref,
                           int ref_stride,
                           int16_t out[16]);

/*
VP8 forward WHT for the 16 luma DC coefficients (Y2), mirroring libwebp's
scalar FTransformWHT_C.

Input layout matches libwebp's use with `int16_t tmp[16][16]`:
- in points at tmp[0][0]
- the 16 DC values are read from in[k*16 + 0] for k=0..15

out receives 16 coefficients.
*/
void enc_vp8_ftransform_wht(const int16_t* in, int16_t out[16]);

#ifdef __cplusplus
}
#endif
