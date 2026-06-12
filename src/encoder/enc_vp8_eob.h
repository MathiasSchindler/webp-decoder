#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
Builds a VP8 keyframe payload that decodes, for arbitrary dimensions.

Current M4 behavior:
- Keyframe only
- Width/height are encoded exactly (decoder crops); macroblock grid is ceil/16 padded.
- Intra DC_PRED for Y and UV for every macroblock.
- All coefficients are EOB (no residual), so output is a deterministic flat image.
- Single token partition.

On success:
- Allocates *out_payload via malloc; caller must free().
- Writes payload size to *out_size.

Returns 0 on success, -1 on failure.
*/
int enc_vp8_build_keyframe_dc_eob(uint32_t width, uint32_t height, uint8_t** out_payload, size_t* out_size);

#ifdef __cplusplus
}
#endif
