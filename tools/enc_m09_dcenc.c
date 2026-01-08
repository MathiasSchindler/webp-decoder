#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/enc-m00_png/enc_png.h"
#include "../src/enc-m01_riff/enc_riff.h"
#include "../src/enc-m04_yuv/enc_rgb_to_yuv.h"
#include "../src/enc-m07_tokens/enc_vp8_tokens.h"
#include "../src/enc-m08_recon/enc_recon.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--q <0..100>] [--loopfilter] <in.png> <out.webp>\n"
	        "\n"
	        "Encodes a VP8 keyframe (lossy) into a simple WebP container.\n"
	        "Current mode: DC_PRED (Y+UV) with in-loop reconstruction.\n"
	        "--loopfilter enables deterministic loopfilter header params derived from the chosen qindex.\n",
	        argv0);
}

static int parse_int(const char* s, int* out) {
	char* end = NULL;
	long v = strtol(s, &end, 10);
	if (!s[0] || (end && *end)) return -1;
	if (v < -2147483647L || v > 2147483647L) return -1;
	*out = (int)v;
	return 0;
}

int main(int argc, char** argv) {
	int quality = 75;
	int enable_loopfilter = 0;

	int argi = 1;
	while (argi < argc) {
		if (argi + 1 < argc && strcmp(argv[argi], "--q") == 0) {
			if (parse_int(argv[argi + 1], &quality) != 0) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (strcmp(argv[argi], "--loopfilter") == 0 || strcmp(argv[argi], "--lf") == 0) {
			enable_loopfilter = 1;
			argi += 1;
			continue;
		}
		break;
	}
	if (argc - argi != 2) {
		usage(argv[0]);
		return 2;
	}
	const char* in_path = argv[argi++];
	const char* out_path = argv[argi++];

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

	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	uint8_t qindex = 0;
	if (enc_vp8_encode_dc_pred_inloop(&yuv, quality, &coeffs, &coeffs_count, &qindex) != 0) {
		fprintf(stderr, "%s: enc_vp8_encode_dc_pred_inloop failed (errno=%d)\n", in_path, errno);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enable_loopfilter) {
		EncVp8LoopFilterParams lf;
		enc_vp8_loopfilter_from_qindex(qindex, &lf);
		if (enc_vp8_build_keyframe_dc_coeffs_ex(img.width,
		                                      img.height,
		                                      qindex,
		                                      0,
		                                      0,
		                                      0,
		                                      0,
		                                      0,
		                                      &lf,
		                                      coeffs,
		                                      coeffs_count,
		                                      &vp8,
		                                      &vp8_size) != 0) {
			fprintf(stderr, "%s: enc_vp8_build_keyframe_dc_coeffs_ex failed (errno=%d)\n", in_path, errno);
			free(coeffs);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	} else {
		if (enc_vp8_build_keyframe_dc_coeffs(img.width,
		                                    img.height,
		                                    qindex,
		                                    0,
		                                    0,
		                                    0,
		                                    0,
		                                    0,
		                                    coeffs,
		                                    coeffs_count,
		                                    &vp8,
		                                    &vp8_size) != 0) {
			fprintf(stderr, "%s: enc_vp8_build_keyframe_dc_coeffs failed (errno=%d)\n", in_path, errno);
			free(coeffs);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	}

	if (!vp8 || vp8_size == 0) {
		fprintf(stderr, "%s: VP8 payload build failed (empty output)\n", in_path);
		free(coeffs);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	if (enc_webp_write_vp8_file(out_path, vp8, vp8_size) != 0) {
		fprintf(stderr, "%s: enc_webp_write_vp8_file failed (errno=%d)\n", out_path, errno);
		free(vp8);
		free(coeffs);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	free(vp8);
	free(coeffs);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);
	return 0;
}
