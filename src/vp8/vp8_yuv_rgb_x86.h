#pragma once

#include <stdint.h>

#if defined(__SSE2__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(VP8_DISABLE_X86_SIMD)
#define VP8_YUV_RGB_HAVE_SSE2 1

void vp8_yuv_to_rgb4_sse2(uint8_t y0,
                          uint8_t u0,
                          uint8_t v0,
                          uint8_t y1,
                          uint8_t u1,
                          uint8_t v1,
                          uint8_t y2,
                          uint8_t u2,
                          uint8_t v2,
                          uint8_t y3,
                          uint8_t u3,
                          uint8_t v3,
                          uint8_t* dst0,
                          uint8_t* dst1,
                          uint8_t* dst2,
                          uint8_t* dst3);
#endif
