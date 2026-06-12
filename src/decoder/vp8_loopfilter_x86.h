#pragma once

#include <stdint.h>

#if defined(__SSE2__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(VP8_DISABLE_X86_SIMD)
#define VP8_LOOPFILTER_HAVE_SSE2 1

void vp8_filter_h_edge_simple_sse2(uint8_t* src_q0, int stride, int filter_limit);
void vp8_filter_mb_h_edge_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold);
void vp8_filter_subblock_h_edge_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold);
#endif
