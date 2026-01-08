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
	        "Usage: %s [--q <0..100>] [--loopfilter] [--dump-modes <out.bin>] <in.png> <out.webp>\n"
	        "\n"
	        "Encodes a VP8 keyframe (lossy) into a simple WebP container.\n"
	        "Luma mode: per-macroblock I16 (DC/V/H/TM) chosen by SAD against predictors\n"
	        "built from reconstructed neighbors.\n"
	        "Chroma mode: per-macroblock UV (DC/V/H/TM) chosen by SAD against predictors\n"
	        "built from reconstructed neighbors.\n"
	        "--loopfilter enables deterministic loopfilter header params derived from the chosen qindex.\n"
	        "--dump-modes writes [y_modes][uv_modes] as raw bytes, each length mb_total.\n",
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

static int write_file(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f) return -1;
	if (size) {
		if (fwrite(data, 1, size, f) != size) {
			fclose(f);
			errno = EIO;
			return -1;
		}
	}
	if (fclose(f) != 0) return -1;
	return 0;
}

int main(int argc, char** argv) {
	int quality = 75;
	const char* dump_modes_path = NULL;
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
		if (argi + 1 < argc && strcmp(argv[argi], "--dump-modes") == 0) {
			dump_modes_path = argv[argi + 1];
			argi += 2;
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

	uint8_t* y_modes = NULL;
	size_t y_modes_count = 0;
	uint8_t* uv_modes = NULL;
	size_t uv_modes_count = 0;
	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	uint8_t qindex = 0;
	if (enc_vp8_encode_i16x16_uv_sad_inloop(
	        &yuv, quality, &y_modes, &y_modes_count, &uv_modes, &uv_modes_count, &coeffs, &coeffs_count, &qindex) != 0) {
		fprintf(stderr, "%s: enc_vp8_encode_i16x16_uv_sad_inloop failed (errno=%d)\n", in_path, errno);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	if (dump_modes_path) {
		// Dump format: [y_modes bytes][uv_modes bytes], each length mb_total.
		size_t total = y_modes_count + uv_modes_count;
		uint8_t* both = (uint8_t*)malloc(total);
		if (!both) {
			fprintf(stderr, "%s: failed to allocate mode dump buffer (errno=%d)\n", in_path, errno);
			free(uv_modes);
			free(coeffs);
			free(y_modes);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
		memcpy(both, y_modes, y_modes_count);
		memcpy(both + y_modes_count, uv_modes, uv_modes_count);
		if (write_file(dump_modes_path, both, total) != 0) {
			fprintf(stderr, "%s: failed to write modes to %s (errno=%d)\n", in_path, dump_modes_path, errno);
			free(both);
			free(uv_modes);
			free(coeffs);
			free(y_modes);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
		free(both);
	}

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enable_loopfilter) {
		EncVp8LoopFilterParams lf;
		enc_vp8_loopfilter_from_qindex(qindex, &lf);
		if (enc_vp8_build_keyframe_i16_coeffs_ex(img.width,
		                                        img.height,
		                                        qindex,
		                                        0,
		                                        0,
		                                        0,
		                                        0,
		                                        0,
		                                        y_modes,
		                                        uv_modes,
		                                        &lf,
		                                        coeffs,
		                                        coeffs_count,
		                                        &vp8,
		                                        &vp8_size) != 0) {
			fprintf(stderr, "%s: enc_vp8_build_keyframe_i16_coeffs_ex failed (errno=%d)\n", in_path, errno);
			free(uv_modes);
			free(coeffs);
			free(y_modes);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	} else {
		if (enc_vp8_build_keyframe_i16_coeffs(img.width,
		                                     img.height,
		                                     qindex,
		                                     0,
		                                     0,
		                                     0,
		                                     0,
		                                     0,
		                                     y_modes,
		                                     uv_modes,
		                                     coeffs,
		                                     coeffs_count,
		                                     &vp8,
		                                     &vp8_size) != 0) {
			fprintf(stderr, "%s: enc_vp8_build_keyframe_i16_coeffs failed (errno=%d)\n", in_path, errno);
			free(uv_modes);
			free(coeffs);
			free(y_modes);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	}

	if (enc_webp_write_vp8_file(out_path, vp8, vp8_size) != 0) {
		fprintf(stderr, "%s: enc_webp_write_vp8_file failed (errno=%d)\n", out_path, errno);
		free(vp8);
		free(uv_modes);
		free(coeffs);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	free(vp8);
	free(uv_modes);
	free(coeffs);
	free(y_modes);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);
	return 0;
}
