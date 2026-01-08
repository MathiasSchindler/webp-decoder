#pragma once

#include <stdint.h>

#include "../common/os.h"

typedef struct {
	uint8_t color_space;      // 0 or 1
	uint8_t clamp_type;       // 0 or 1

	uint8_t use_segment;      // 0 or 1

	uint8_t simple_filter;    // 0 or 1
	uint8_t filter_level;     // 0..63
	uint8_t sharpness;        // 0..7
	uint8_t use_lf_delta;     // 0 or 1

	uint8_t log2_partitions;  // 0..3
	uint8_t total_partitions; // 1,2,4,8

	// Partition sizes in bytes. part_sizes[0] is the first partition length
	// from the frame tag ("Part. 0 length" in webpinfo).
	// For total_partitions > 1, part_sizes[1..n-1] are DCT token partitions.
	uint32_t part_sizes[8];

	uint8_t base_q;           // 0..127
	int8_t dq_y1_dc;
	int8_t dq_y2_dc;
	int8_t dq_y2_ac;
	int8_t dq_uv_dc;
	int8_t dq_uv_ac;
} Vp8FrameHeaderBasic;

// Parses a subset of the VP8 frame header fields that webpinfo prints.
// Input is the full VP8 payload bytes (from the WebP 'VP8 ' chunk).
// Returns 0 on success.
int vp8_parse_frame_header_basic(ByteSpan vp8_payload, Vp8FrameHeaderBasic* out);
