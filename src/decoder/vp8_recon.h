#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../common/os.h"
#include "vp8_header.h"
#include "vp8_tokens.h"

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t stride_y;
	uint32_t stride_uv;
	uint8_t* y;
	uint8_t* u;
	uint8_t* v;
} Yuv420Image;

typedef struct Vp8LoopfilterProfile Vp8LoopfilterProfile;

typedef struct {
	uint64_t y_prediction_ns[5];      // DC, V, H, TM, B_PRED aggregate
	uint64_t uv_prediction_ns[4];     // DC, V, H, TM aggregate for U+V
	uint64_t b_prediction_ns[10];     // B_DC, B_TM, B_VE, B_HE, B_LD, B_RD, B_VR, B_VL, B_HD, B_HU
	uint64_t idct_ns[3];              // zero, DC-only, has AC
	uint64_t copy_block_ns;
	uint64_t add_constant_ns;
	uint64_t add_residue_ns;
	uint64_t y2_wht_ns[3];            // zero, DC-only, has AC
	uint64_t block_class[4][3];       // 0=Y, 1=Y2, 2=U, 3=V; zero, DC-only, has AC
	uint64_t y_prediction_calls[5];
	uint64_t uv_prediction_calls[4];
	uint64_t b_prediction_calls[10];
	uint64_t copy_block_calls;
	uint64_t add_constant_calls;
	uint64_t add_residue_calls;
} Vp8ReconProfile;

int yuv420_alloc(Yuv420Image* img, uint32_t width, uint32_t height);
void yuv420_free(Yuv420Image* img);

// Reconstructs an intra (key) frame into planar 4:2:0 (I420) buffers.
// Loop filter is NOT applied (matches Milestone-6 output).
int vp8_reconstruct_keyframe_yuv(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out);

// Reconstructs an intra (key) frame and applies the in-loop deblocking filter.
int vp8_reconstruct_keyframe_yuv_filtered(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out);
int vp8_reconstruct_keyframe_yuv_filtered_profiled(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded,
                                                   Yuv420Image* out, Vp8ReconProfile* recon_profile,
                                                   Vp8LoopfilterProfile* loopfilter_profile);

// Decode + reconstruct keyframes while streaming coefficients by macroblock.
// These avoid frame-sized coefficient arrays; syntax arrays are still retained
// long enough for loopfilter skip/segment decisions.
int vp8_decode_reconstruct_keyframe_yuv(ByteSpan vp8_payload, Yuv420Image* out);
int vp8_decode_reconstruct_keyframe_yuv_filtered(ByteSpan vp8_payload, Yuv420Image* out);
