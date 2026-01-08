#include "vp8_frame_header_basic.h"

#include <errno.h>

#include "../m02_vp8_header/vp8_header.h"
#include "../m03_bool_decoder/bool_decoder.h"

static int8_t decode_q_delta(BoolDecoder* d) {
	// Present flag + 4-bit magnitude + sign bit.
	if (bool_decode_bool(d, 128) == 0) return 0;
	int32_t v = bool_decode_sint(d, 4);
	if (v < -128) v = -128;
	if (v > 127) v = 127;
	return (int8_t)v;
}

static uint32_t load_u24_le(const uint8_t* p) {
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int vp8_parse_frame_header_basic(ByteSpan vp8_payload, Vp8FrameHeaderBasic* out) {
	if (!out) return -1;
	*out = (Vp8FrameHeaderBasic){0};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0) {
		errno = EINVAL;
		return -1;
	}

	// First partition begins immediately after 10-byte uncompressed header.
	const size_t uncompressed = 10;
	if (vp8_payload.size < uncompressed + kf.first_partition_len) {
		errno = EINVAL;
		return -1;
	}

	ByteSpan part0 = {
		.data = vp8_payload.data + uncompressed,
		.size = kf.first_partition_len,
	};

	BoolDecoder d;
	if (bool_decoder_init(&d, part0) != 0) return -1;

	out->color_space = (uint8_t)bool_decode_bool(&d, 128);
	out->clamp_type = (uint8_t)bool_decode_bool(&d, 128);

	// Segmentation (RFC 6386 9.3)
	out->use_segment = (uint8_t)bool_decode_bool(&d, 128);
	if (out->use_segment) {
		int update_mb_segmentation_map = bool_decode_bool(&d, 128);
		int update_segment_feature_data = bool_decode_bool(&d, 128);
		if (update_segment_feature_data) {
			(void)bool_decode_bool(&d, 128); // segment_feature_mode
			// Quantizer updates: 4 segments
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) (void)bool_decode_sint(&d, 7);
			}
			// Loop filter updates: 4 segments
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) (void)bool_decode_sint(&d, 6);
			}
		}
		if (update_mb_segmentation_map) {
			for (int i = 0; i < 3; i++) {
				if (bool_decode_bool(&d, 128)) (void)bool_decode_literal(&d, 8);
			}
		}
	}

	// Loop filter (RFC 6386 9.4)
	out->simple_filter = (uint8_t)bool_decode_bool(&d, 128);
	out->filter_level = (uint8_t)bool_decode_literal(&d, 6);
	out->sharpness = (uint8_t)bool_decode_literal(&d, 3);
	out->use_lf_delta = (uint8_t)bool_decode_bool(&d, 128);
	if (out->use_lf_delta) {
		int update = bool_decode_bool(&d, 128);
		if (update) {
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) (void)bool_decode_sint(&d, 6);
			}
			for (int i = 0; i < 4; i++) {
				if (bool_decode_bool(&d, 128)) (void)bool_decode_sint(&d, 6);
			}
		}
	}

	// Token partitions (RFC 6386 9.5)
	out->log2_partitions = (uint8_t)bool_decode_literal(&d, 2);
	out->total_partitions = (uint8_t)(1u << out->log2_partitions);
	out->part_sizes[0] = kf.first_partition_len;

	// Quantization (RFC 6386 9.6)
	out->base_q = (uint8_t)bool_decode_literal(&d, 7);
	out->dq_y1_dc = decode_q_delta(&d);
	out->dq_y2_dc = decode_q_delta(&d);
	out->dq_y2_ac = decode_q_delta(&d);
	out->dq_uv_dc = decode_q_delta(&d);
	out->dq_uv_ac = decode_q_delta(&d);

	// Partition size table for token partitions is stored in bytes after partition 0.
	// Layout:
	//   [uncompressed header 10 bytes]
	//   [partition 0 data: first_partition_len bytes]
	//   [if n>1: (n-1)*3 bytes sizes for partitions 1..n-2]
	//   [token partitions 1..n-1 consecutive]
	const uint8_t n = out->total_partitions;
	if (n > 1) {
		size_t table_off = uncompressed + (size_t)kf.first_partition_len;
		size_t table_len = (size_t)(n - 1u) * 3u;
		if (vp8_payload.size < table_off + table_len) {
			errno = EINVAL;
			return -1;
		}
		uint64_t sum = 0;
		for (uint8_t i = 0; i + 1u < n; i++) {
			uint32_t sz = load_u24_le(vp8_payload.data + table_off + (size_t)i * 3u);
			out->part_sizes[1u + i] = sz;
			sum += sz;
		}
		size_t token_data_off = table_off + table_len;
		if (vp8_payload.size < token_data_off) {
			errno = EINVAL;
			return -1;
		}
		size_t token_data_len = vp8_payload.size - token_data_off;
		if (sum > token_data_len) {
			errno = EINVAL;
			return -1;
		}
		out->part_sizes[n - 1u] = (uint32_t)(token_data_len - (size_t)sum);
	}

	return 0;
}
