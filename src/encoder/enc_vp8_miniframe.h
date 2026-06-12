#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
Builds a minimal VP8 keyframe payload (no WebP container) that should decode.

Current constraints (M3 bring-up):
- Keyframe only
- Dimensions must be 16x16
- Single macroblock (1x1)
- Intra DC_PRED for Y and UV
- All coefficients are EOB (no residual)

On success:
- Allocates *out_payload via malloc; caller must free().
- Writes payload size to *out_size.

Returns 0 on success, -1 on failure.
*/
int enc_vp8_build_minikeyframe_16x16(uint8_t** out_payload, size_t* out_size);

#ifdef __cplusplus
}
#endif
