#include "enc-m00_png/enc_png.h"
#include "enc-m01_riff/enc_riff.h"
#include "enc-m04_yuv/enc_rgb_to_yuv.h"
#include "enc-m08_recon/enc_recon.h"
#include "enc-m07_tokens/enc_vp8_tokens.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

// Ultra/nolibc entrypoint: keep CLI minimal (like decoder_nolibc_ultra).
// Usage: encoder_nolibc_ultra <in.png> <out.webp>

int main(int argc, char** argv) {
	if (argc != 3) return 2;
	const char* in_path = argv[1];
	const char* out_path = argv[2];

	EncPngImage img;
	if (enc_png_read_file(in_path, &img) != 0) return 1;
	if (!(img.channels == 3 || img.channels == 4)) {
		enc_png_free(&img);
		errno = EINVAL;
		return 1;
	}

	EncYuv420Image yuv;
	const uint32_t stride = img.width * (uint32_t)img.channels;
	if (enc_yuv420_from_rgb_libwebp(img.data, img.width, img.height, stride, img.channels, &yuv) != 0) {
		enc_png_free(&img);
		return 1;
	}

	// Fixed defaults for ultra build.
	const int quality = 75;

	uint8_t* y_modes = NULL;
	size_t y_modes_count = 0;
	uint8_t* b_modes = NULL;
	size_t b_modes_count = 0;
	uint8_t* uv_modes = NULL;
	size_t uv_modes_count = 0;
	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	uint8_t qindex = 0;

	int rc = enc_vp8_encode_bpred_uv_sad_inloop(&yuv,
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
	if (rc != 0) {
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
	if (rc != 0 || !vp8 || vp8_size == 0) {
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
