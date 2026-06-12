#include "vp8_header.h"

#include <errno.h>

static uint32_t load_u24_le(const uint8_t* p) {
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint16_t load_u16_le(const uint8_t* p) {
	return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}

int vp8_parse_keyframe_header(ByteSpan vp8_payload, Vp8KeyFrameHeader* out) {
	if (!out) return -1;
	*out = (Vp8KeyFrameHeader){0};

	// Frame tag (3 bytes) + start code (3 bytes) + w/h (2+2 bytes) = 10 bytes.
	if (!vp8_payload.data || vp8_payload.size < 10) {
		errno = EINVAL;
		return -1;
	}

	uint32_t tag = load_u24_le(vp8_payload.data);
	int key_frame_bit = (int)(tag & 1u); // 0 => key frame
	out->is_key_frame = key_frame_bit ? 0 : 1;
	out->profile = (uint8_t)((tag >> 1) & 7u);
	out->show_frame = (int)((tag >> 4) & 1u);
	out->first_partition_len = (tag >> 5) & 0x7FFFFu;

	// Key frame header only defined for key frames.
	if (!out->is_key_frame) {
		errno = EINVAL;
		return -1;
	}

	const uint8_t* p = vp8_payload.data + 3;
	out->start_code_ok = (p[0] == 0x9d && p[1] == 0x01 && p[2] == 0x2a) ? 1 : 0;
	if (!out->start_code_ok) {
		errno = EINVAL;
		return -1;
	}
	p += 3;

	uint16_t w = load_u16_le(p);
	uint16_t h = load_u16_le(p + 2);
	out->width = (uint16_t)(w & 0x3FFFu);
	out->x_scale = (uint8_t)((w >> 14) & 0x3u);
	out->height = (uint16_t)(h & 0x3FFFu);
	out->y_scale = (uint8_t)((h >> 14) & 0x3u);

	// Basic sanity.
	if (out->width == 0 || out->height == 0) {
		errno = EINVAL;
		return -1;
	}

	// Partition length must fit in remaining payload.
	// (We don't parse partitions yet, but we can bound-check to catch obvious corruption.)
	size_t header_bytes = 10;
	if ((size_t)out->first_partition_len > (vp8_payload.size - header_bytes)) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}
