#pragma once

#include <stdint.h>

void vp8_inv_wht4x4(const int16_t* input, int16_t* output);
void vp8_inv_wht4x4_dc_only(int16_t dc, int16_t* output);
void vp8_inv_dct4x4(const int16_t* input, int16_t* output);
void vp8_inv_dct4x4_dc_only(int16_t dc, int16_t* output);
