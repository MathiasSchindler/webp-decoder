#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t use_simple;   // 0/1
	uint8_t level;        // 0..63
	uint8_t sharpness;    // 0..7
	uint8_t use_lf_delta; // 0/1 (not used yet)
} EncVp8LoopFilterParams;

// Deterministic loopfilter parameter selection.
// This is intentionally a simple heuristic for now, but mirrors the general
// VP8 intent: stronger filtering at lower quality (higher qindex).
void enc_vp8_loopfilter_from_qindex(uint8_t qindex, EncVp8LoopFilterParams* out);

#ifdef __cplusplus
}
#endif
