#pragma once

#include <stdint.h>

#include "vp8_recon.h"
#include "vp8_tokens.h"

typedef struct Vp8LoopfilterProfile {
	uint64_t disabled_or_skipped_mbs;
	uint64_t simple_vertical_mb_luma_ns;
	uint64_t simple_vertical_sub_luma_ns;
	uint64_t simple_horizontal_mb_luma_ns;
	uint64_t simple_horizontal_sub_luma_ns;
	uint64_t normal_vertical_mb_luma_ns;
	uint64_t normal_vertical_mb_chroma_ns;
	uint64_t normal_vertical_sub_luma_ns;
	uint64_t normal_vertical_sub_chroma_ns;
	uint64_t normal_horizontal_mb_luma_ns;
	uint64_t normal_horizontal_mb_chroma_ns;
	uint64_t normal_horizontal_sub_luma_ns;
	uint64_t normal_horizontal_sub_chroma_ns;
	uint64_t simple_vertical_mb_luma_calls;
	uint64_t simple_vertical_sub_luma_calls;
	uint64_t simple_horizontal_mb_luma_calls;
	uint64_t simple_horizontal_sub_luma_calls;
	uint64_t normal_vertical_mb_luma_calls;
	uint64_t normal_vertical_mb_chroma_calls;
	uint64_t normal_vertical_sub_luma_calls;
	uint64_t normal_vertical_sub_chroma_calls;
	uint64_t normal_horizontal_mb_luma_calls;
	uint64_t normal_horizontal_mb_chroma_calls;
	uint64_t normal_horizontal_sub_luma_calls;
	uint64_t normal_horizontal_sub_chroma_calls;
} Vp8LoopfilterProfile;

// Applies the VP8 in-loop deblocking filter to a reconstructed keyframe.
//
// The filter operates in-place on the *macroblock-aligned* reconstruction buffer.
// The caller should apply the filter before cropping to visible width/height.
//
// Returns 0 on success.
int vp8_loopfilter_apply_keyframe(Yuv420Image* padded_img, const Vp8DecodedFrame* decoded);
int vp8_loopfilter_apply_keyframe_profiled(Yuv420Image* padded_img, const Vp8DecodedFrame* decoded,
                                           Vp8LoopfilterProfile* profile);
