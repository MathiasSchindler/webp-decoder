#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../enc-m04_yuv/enc_rgb_to_yuv.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Builds a deterministic coefficient dump for DC intra mode (I16 + UV DC) over the
macroblock grid (ceil(width/16), ceil(height/16)).

Output format (binary, little-endian int16 coefficients), per macroblock:
- Y2 (16)
- Y blocks (16 * 16) with per-block DC set to 0
- U blocks (4 * 16)
- V blocks (4 * 16)

Returns 0 on success and allocates *out_bytes via malloc.
Caller must free(*out_bytes).
*/
int enc_vp8_dc_transformdump(const EncYuv420Image* yuv,
                            uint32_t mb_cols,
                            uint32_t mb_rows,
                            uint8_t** out_bytes,
                            size_t* out_size);

#ifdef __cplusplus
}
#endif
