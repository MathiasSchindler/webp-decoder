#pragma once

#include <stdint.h>

void vp8_bpred4x4(uint8_t* dst, uint32_t stride, const uint8_t* above, const uint8_t* left, uint8_t mode);
