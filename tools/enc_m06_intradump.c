#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/enc-m00_png/enc_png.h"
#include "../src/enc-m04_yuv/enc_rgb_to_yuv.h"
#include "../src/enc-m05_intra/enc_intra_dc.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s <in.png> [out.bin|-]\n"
	        "\n"
	        "Dumps DC intra (I16 + UV DC) forward-transform coefficient buffers for\n"
	        "each macroblock in raster order. Output is binary int16 little-endian.\n",
	        argv0);
}

static int fwrite_all(FILE* f, const void* data, size_t n) {
	return fwrite(data, 1, n, f) == n ? 0 : -1;
}

int main(int argc, char** argv) {
	int argi = 1;
	if (argc - argi < 1 || argc - argi > 2) {
		usage(argv[0]);
		return 2;
	}

	const char* in_path = argv[argi++];
	const char* out_path = (argc - argi == 1) ? argv[argi++] : "-";

	EncPngImage img;
	if (enc_png_read_file(in_path, &img) != 0) {
		fprintf(stderr, "enc_png_read_file failed for %s (errno=%d)\n", in_path, errno);
		return 1;
	}

	if (!(img.channels == 3 || img.channels == 4)) {
		fprintf(stderr, "%s: unsupported channels=%u\n", in_path, img.channels);
		enc_png_free(&img);
		return 1;
	}

	EncYuv420Image yuv;
	const uint32_t stride = img.width * (uint32_t)img.channels;
	if (enc_yuv420_from_rgb_libwebp(img.data, img.width, img.height, stride, img.channels, &yuv) != 0) {
		fprintf(stderr, "%s: RGB->YUV failed (errno=%d)\n", in_path, errno);
		enc_png_free(&img);
		return 1;
	}

	const uint32_t mb_cols = (img.width + 15u) >> 4;
	const uint32_t mb_rows = (img.height + 15u) >> 4;

	uint8_t* dump = NULL;
	size_t dump_size = 0;
	if (enc_vp8_dc_transformdump(&yuv, mb_cols, mb_rows, &dump, &dump_size) != 0) {
		fprintf(stderr, "%s: enc_vp8_dc_transformdump failed (errno=%d)\n", in_path, errno);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	FILE* out = stdout;
	if (strcmp(out_path, "-") != 0) {
		out = fopen(out_path, "wb");
		if (!out) {
			fprintf(stderr, "fopen(%s) failed (errno=%d)\n", out_path, errno);
			free(dump);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	}

	fprintf(stderr,
	        "%s: %ux%u -> %ux%u MBs -> %zu bytes coeff dump\n",
	        in_path,
	        img.width,
	        img.height,
	        mb_cols,
	        mb_rows,
	        dump_size);

	int ok = fwrite_all(out, dump, dump_size);
	if (out != stdout) fclose(out);

	free(dump);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);

	if (ok != 0) {
		fprintf(stderr, "write failed\n");
		return 1;
	}
	return 0;
}
