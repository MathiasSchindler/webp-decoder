#pragma once

#include <stdint.h>

void vp8_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* dst3);
void vp8_upsample_rgb_line_pair(const uint8_t* top_y,
                                const uint8_t* bottom_y,
                                const uint8_t* top_u,
                                const uint8_t* top_v,
                                const uint8_t* cur_u,
                                const uint8_t* cur_v,
                                uint8_t* top_dst,
                                uint8_t* bottom_dst,
                                uint32_t len);
