#pragma once

#include <stdint.h>

#include "../m06_recon/vp8_recon.h"
#include "../m05_tokens/vp8_tokens.h"

// Applies the VP8 in-loop deblocking filter to a reconstructed keyframe.
//
// The filter operates in-place on the *macroblock-aligned* reconstruction buffer.
// The caller should apply the filter before cropping to visible width/height.
//
// Returns 0 on success.
int vp8_loopfilter_apply_keyframe(Yuv420Image* padded_img, const Vp8DecodedFrame* decoded);
