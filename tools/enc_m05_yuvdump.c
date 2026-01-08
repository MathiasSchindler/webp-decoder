#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/enc-m00_png/enc_png.h"
#include "../src/enc-m04_yuv/enc_rgb_to_yuv.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--i420|--y|--u|--v] <in.png> [out.raw|-]\n"
	        "\n"
	        "Writes raw bytes. Default: --i420 (Y then U then V).\n",
	        argv0);
}

typedef enum DumpMode {
	DUMP_I420,
	DUMP_Y,
	DUMP_U,
	DUMP_V,
} DumpMode;

static int fwrite_all(FILE* f, const void* data, size_t n) {
	return fwrite(data, 1, n, f) == n ? 0 : -1;
}

int main(int argc, char** argv) {
	DumpMode mode = DUMP_I420;
	int argi = 1;
	if (argi < argc && strcmp(argv[argi], "--i420") == 0) {
		mode = DUMP_I420;
		argi++;
	} else if (argi < argc && strcmp(argv[argi], "--y") == 0) {
		mode = DUMP_Y;
		argi++;
	} else if (argi < argc && strcmp(argv[argi], "--u") == 0) {
		mode = DUMP_U;
		argi++;
	} else if (argi < argc && strcmp(argv[argi], "--v") == 0) {
		mode = DUMP_V;
		argi++;
	}

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

	FILE* out = stdout;
	if (strcmp(out_path, "-") != 0) {
		out = fopen(out_path, "wb");
		if (!out) {
			fprintf(stderr, "fopen(%s) failed (errno=%d)\n", out_path, errno);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	}

	const uint32_t uv_w = (img.width + 1u) >> 1;
	const uint32_t uv_h = (img.height + 1u) >> 1;
	const size_t y_bytes = (size_t)img.width * (size_t)img.height;
	const size_t uv_bytes = (size_t)uv_w * (size_t)uv_h;

	fprintf(stderr, "%s: %ux%u -> Y=%zu U/V=%zux2 (limited-range, libwebp-matched)\n",
	        in_path,
	        img.width,
	        img.height,
	        y_bytes,
	        uv_bytes);

	int ok = 0;
	if (mode == DUMP_I420) {
		if (fwrite_all(out, yuv.y, y_bytes) != 0) ok = -1;
		if (ok == 0 && fwrite_all(out, yuv.u, uv_bytes) != 0) ok = -1;
		if (ok == 0 && fwrite_all(out, yuv.v, uv_bytes) != 0) ok = -1;
	} else if (mode == DUMP_Y) {
		ok = fwrite_all(out, yuv.y, y_bytes);
	} else if (mode == DUMP_U) {
		ok = fwrite_all(out, yuv.u, uv_bytes);
	} else if (mode == DUMP_V) {
		ok = fwrite_all(out, yuv.v, uv_bytes);
	}

	if (out != stdout) fclose(out);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);

	if (ok != 0) {
		fprintf(stderr, "write failed\n");
		return 1;
	}

	return 0;
}
