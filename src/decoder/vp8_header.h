#pragma once

#include <stdint.h>

#include "../common/os.h"

typedef struct {
	int is_key_frame;            // 1 if key frame, 0 otherwise
	uint8_t profile;             // VP8 profile/version (0..3)
	int show_frame;              // 1 if displayable, 0 otherwise
	uint32_t first_partition_len; // bytes

	int start_code_ok;           // 1 if 0x9d 0x01 0x2a
	uint16_t width;
	uint16_t height;
	uint8_t x_scale;             // 0..3
	uint8_t y_scale;             // 0..3
} Vp8KeyFrameHeader;

// Parses only the VP8 frame tag + key-frame header (RFC 6386).
// Input is the payload bytes of the WebP 'VP8 ' chunk.
// Returns 0 on success.
int vp8_parse_keyframe_header(ByteSpan vp8_payload, Vp8KeyFrameHeader* out);
