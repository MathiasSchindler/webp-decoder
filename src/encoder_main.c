#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enc-m00_png/enc_png.h"
#include "enc-m01_riff/enc_riff.h"
#include "enc-m04_yuv/enc_rgb_to_yuv.h"
#include "enc-m07_tokens/enc_vp8_tokens.h"
#include "enc-m08_filter/enc_loopfilter.h"
#include "enc-m08_recon/enc_recon.h"

typedef enum {
	ENC_MODE_BPRED = 0,
	ENC_MODE_I16 = 1,
	ENC_MODE_DC = 2,
} EncMode;

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--q <0..100>] [--mode <bpred|i16|dc>] [--loopfilter] <in.png> <out.webp>\n"
	        "\n"
	        "Standalone VP8 keyframe (lossy) encoder producing a simple WebP container.\n"
	        "\n"
	        "Options:\n"
	        "  --q <0..100>           Quality (mapped to VP8 qindex). Default: 75\n"
	        "  --mode <bpred|i16|dc>  Intra mode strategy. Default: bpred\n"
	        "  --loopfilter | --lf    Write deterministic loopfilter header params derived from qindex\n",
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

static int parse_mode(const char* s, EncMode* out) {
	if (strcmp(s, "bpred") == 0) {
		*out = ENC_MODE_BPRED;
		return 0;
	}
	if (strcmp(s, "i16") == 0 || strcmp(s, "i16x16") == 0) {
		*out = ENC_MODE_I16;
		return 0;
	}
	if (strcmp(s, "dc") == 0 || strcmp(s, "dc_pred") == 0) {
		*out = ENC_MODE_DC;
		return 0;
	}
	return -1;
}

int main(int argc, char** argv) {
	int quality = 75;
	int enable_loopfilter = 0;
	EncMode mode = ENC_MODE_BPRED;

	int argi = 1;
	while (argi < argc) {
		if (argi + 1 < argc && strcmp(argv[argi], "--q") == 0) {
			if (parse_int(argv[argi + 1], &quality) != 0 || quality < 0 || quality > 100) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--mode") == 0) {
			if (parse_mode(argv[argi + 1], &mode) != 0) {
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

	uint8_t* y_modes = NULL;
	size_t y_modes_count = 0;
	uint8_t* b_modes = NULL;
	size_t b_modes_count = 0;
	uint8_t* uv_modes = NULL;
	size_t uv_modes_count = 0;
	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	uint8_t qindex = 0;

	int rc = 0;
	if (mode == ENC_MODE_DC) {
		rc = enc_vp8_encode_dc_pred_inloop(&yuv, quality, &coeffs, &coeffs_count, &qindex);
	} else if (mode == ENC_MODE_I16) {
		rc = enc_vp8_encode_i16x16_uv_sad_inloop(&yuv,
		                                         quality,
		                                         &y_modes,
		                                         &y_modes_count,
		                                         &uv_modes,
		                                         &uv_modes_count,
		                                         &coeffs,
		                                         &coeffs_count,
		                                         &qindex);
	} else {
		rc = enc_vp8_encode_bpred_uv_sad_inloop(&yuv,
		                                       quality,
		                                       &y_modes,
		                                       &y_modes_count,
		                                       &b_modes,
		                                       &b_modes_count,
		                                       &uv_modes,
		                                       &uv_modes_count,
		                                       &coeffs,
		                                       &coeffs_count,
		                                       &qindex);
	}
	if (rc != 0) {
		fprintf(stderr, "%s: VP8 analysis/quant/recon failed (errno=%d)\n", in_path, errno);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enable_loopfilter) {
		EncVp8LoopFilterParams lf;
		enc_vp8_loopfilter_from_qindex(qindex, &lf);
		if (mode == ENC_MODE_DC) {
			rc = enc_vp8_build_keyframe_dc_coeffs_ex(img.width,
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
			                                     &vp8_size);
		} else if (mode == ENC_MODE_I16) {
			rc = enc_vp8_build_keyframe_i16_coeffs_ex(img.width,
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
			                                      &vp8_size);
		} else {
			rc = enc_vp8_build_keyframe_intra_coeffs_ex(img.width,
			                                        img.height,
			                                        qindex,
			                                        0,
			                                        0,
			                                        0,
			                                        0,
			                                        0,
			                                        y_modes,
			                                        uv_modes,
			                                        b_modes,
			                                        &lf,
			                                        coeffs,
			                                        coeffs_count,
			                                        &vp8,
			                                        &vp8_size);
		}
	} else {
		if (mode == ENC_MODE_DC) {
			rc = enc_vp8_build_keyframe_dc_coeffs(img.width,
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
			                                  &vp8_size);
		} else if (mode == ENC_MODE_I16) {
			rc = enc_vp8_build_keyframe_i16_coeffs(img.width,
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
			                                   &vp8_size);
		} else {
			rc = enc_vp8_build_keyframe_intra_coeffs(img.width,
			                                     img.height,
			                                     qindex,
			                                     0,
			                                     0,
			                                     0,
			                                     0,
			                                     0,
			                                     y_modes,
			                                     uv_modes,
			                                     b_modes,
			                                     coeffs,
			                                     coeffs_count,
			                                     &vp8,
			                                     &vp8_size);
		}
	}

	if (rc != 0 || !vp8 || vp8_size == 0) {
		fprintf(stderr, "%s: VP8 bitstream build failed (errno=%d)\n", in_path, errno);
		free(vp8);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	if (enc_webp_write_vp8_file(out_path, vp8, vp8_size) != 0) {
		fprintf(stderr, "%s: enc_webp_write_vp8_file failed (errno=%d)\n", out_path, errno);
		free(vp8);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	free(vp8);
	free(coeffs);
	free(uv_modes);
	free(b_modes);
	free(y_modes);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);
	return 0;
}
