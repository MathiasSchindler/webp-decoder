#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../common/os.h"
#include "../m02_vp8_header/vp8_header.h"
#include "../m05_tokens/vp8_tokens.h"

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t stride_y;
	uint32_t stride_uv;
	uint8_t* y;
	uint8_t* u;
	uint8_t* v;
} Yuv420Image;

int yuv420_alloc(Yuv420Image* img, uint32_t width, uint32_t height);
void yuv420_free(Yuv420Image* img);

// Reconstructs an intra (key) frame into planar 4:2:0 (I420) buffers.
// Loop filter is NOT applied (matches Milestone-6 output).
int vp8_reconstruct_keyframe_yuv(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out);

// Reconstructs an intra (key) frame and applies the in-loop deblocking filter.
int vp8_reconstruct_keyframe_yuv_filtered(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out);
