#pragma once

#include <stdint.h>

#if defined(__SSE2__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(VP8_DISABLE_X86_SIMD)
#define VP8_YUV_RGB_HAVE_SSE2 1
/*
 * RGB formatting keeps the scalar C math as the reference.  Extra x86 tiers
 * only widen packing/stores for the same fixed-point equations, and disappear
 * entirely when VP8_DISABLE_X86_SIMD is defined.
 */
#if defined(__SSSE3__)
#define VP8_YUV_RGB_HAVE_SSSE3 1
#endif
#if defined(__AVX2__)
#define VP8_YUV_RGB_HAVE_AVX2 1
#endif

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
void vp8_upsample_rgb_line_sse2(const uint8_t* top_y,
                                const uint8_t* top_u,
                                const uint8_t* top_v,
                                const uint8_t* cur_u,
                                const uint8_t* cur_v,
                                uint8_t* top_dst,
                                uint32_t len);
void vp8_upsample_rgb_line_pair_sse2(const uint8_t* top_y,
                                     const uint8_t* bottom_y,
                                     const uint8_t* top_u,
                                     const uint8_t* top_v,
                                     const uint8_t* cur_u,
                                     const uint8_t* cur_v,
                                     uint8_t* top_dst,
                                     uint8_t* bottom_dst,
                                     uint32_t len);
#endif
