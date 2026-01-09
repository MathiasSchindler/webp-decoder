#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../src/enc-m00_png/enc_png.h"

static void usage(const char* argv0) {
	fprintf(stderr, "Usage: %s <in.png> <out.ppm|->\n", argv0);
}

int main(int argc, char** argv) {
	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	const char* in_path = argv[1];
	const char* out_path = argv[2];

	EncPngImage img;
	if (enc_png_read_file(in_path, &img) != 0) {
		fprintf(stderr, "enc_png_read_file failed for %s (errno=%d)\n", in_path, errno);
		return 1;
	}

	FILE* out = stdout;
	if (strcmp(out_path, "-") != 0) {
		out = fopen(out_path, "wb");
		if (!out) {
			fprintf(stderr, "fopen(%s) failed (errno=%d)\n", out_path, errno);
			enc_png_free(&img);
			return 1;
		}
	}

	if (fprintf(out, "P6\n%u %u\n255\n", img.width, img.height) < 0) {
		fprintf(stderr, "write header failed\n");
		enc_png_free(&img);
		if (out != stdout) fclose(out);
		return 1;
	}

	size_t npx = (size_t)img.width * (size_t)img.height;
	if (img.channels == 3) {
		size_t bytes = npx * 3;
		if (fwrite(img.data, 1, bytes, out) != bytes) {
			fprintf(stderr, "write failed\n");
			enc_png_free(&img);
			if (out != stdout) fclose(out);
			return 1;
		}
	} else if (img.channels == 4) {
		for (size_t i = 0; i < npx; i++) {
			const uint8_t* p = img.data + i * 4;
			uint8_t rgb[3] = {p[0], p[1], p[2]};
			if (fwrite(rgb, 1, 3, out) != 3) {
				fprintf(stderr, "write failed\n");
				enc_png_free(&img);
				if (out != stdout) fclose(out);
				return 1;
			}
		}
	} else {
		fprintf(stderr, "unsupported channels=%u\n", img.channels);
		enc_png_free(&img);
		if (out != stdout) fclose(out);
		return 1;
	}

	enc_png_free(&img);
	if (out != stdout) fclose(out);
	return 0;
}