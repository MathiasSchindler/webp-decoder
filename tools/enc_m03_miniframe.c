#include "../src/enc-m01_riff/enc_riff.h"
#include "../src/enc-m03_vp8_headers/enc_vp8_miniframe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(const char* argv0) {
	fprintf(stderr, "Usage: %s <out.webp>\n", argv0);
}

int main(int argc, char** argv) {
	if (argc != 2) {
		usage(argv[0]);
		return 2;
	}

	const char* out_path = argv[1];
	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enc_vp8_build_minikeyframe_16x16(&vp8, &vp8_size) != 0) {
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
