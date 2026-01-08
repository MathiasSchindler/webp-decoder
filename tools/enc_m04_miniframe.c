#include "../src/enc-m01_riff/enc_riff.h"
#include "../src/enc-m04_yuv/enc_vp8_eob.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char* argv0) {
	fprintf(stderr, "Usage: %s <width> <height> <out.webp>\n", argv0);
}

static int parse_u32(const char* s, uint32_t* out) {
	char* end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if (!s[0] || (end && *end)) return -1;
	if (v > 0xFFFFFFFFul) return -1;
	*out = (uint32_t)v;
	return 0;
}

int main(int argc, char** argv) {
	if (argc != 4) {
		usage(argv[0]);
		return 2;
	}

	uint32_t w = 0, h = 0;
	if (parse_u32(argv[1], &w) != 0 || parse_u32(argv[2], &h) != 0) {
		fprintf(stderr, "error: bad width/height\n");
		return 2;
	}
	const char* out_path = argv[3];

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enc_vp8_build_keyframe_dc_eob(w, h, &vp8, &vp8_size) != 0) {
		fprintf(stderr, "error: failed to build VP8 payload (errno=%d)\n", errno);
		return 1;
	}

	int rc = enc_webp_write_vp8_file(out_path, vp8, vp8_size);
	free(vp8);
	if (rc != 0) {
		fprintf(stderr, "error: failed to write WebP (errno=%d)\n", errno);
		return 1;
	}

	return 0;
}
