#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/enc-m00_png/enc_png.h"
#include "../src/enc-m04_yuv/enc_rgb_to_yuv.h"
#include "../src/enc-m05_intra/enc_intra_dc.h"
#include "../src/enc-m06_quant/enc_quant.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--q <0..100>] <in.png> [out.bin|-]\n"
	        "\n"
	        "Dumps DC intra forward-transform coeffs, after scalar quantization.\n"
	        "Output is binary int16 little-endian in macroblock raster order.\n",
	        argv0);
}

static int fwrite_all(FILE* f, const void* data, size_t n) {
	return fwrite(data, 1, n, f) == n ? 0 : -1;
}

static int parse_int(const char* s, int* out) {
	char* end = NULL;
	long v = strtol(s, &end, 10);
	if (!s[0] || (end && *end)) return -1;
	if (v < -2147483647L || v > 2147483647L) return -1;
	*out = (int)v;
	return 0;
}

static void quantize_dump_inplace(uint8_t* dump, size_t dump_size, const EncVp8QuantFactors* qf) {
	// Layout per macroblock, in int16 LE:
	// Y2(16), Y(16*16), U(4*16), V(4*16)
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t bytes_per_mb = coeffs_per_mb * 2;
	if (!dump || dump_size % bytes_per_mb != 0) return;
	const size_t mb_total = dump_size / bytes_per_mb;

	for (size_t mb = 0; mb < mb_total; ++mb) {
		uint8_t* p = dump + mb * bytes_per_mb;

		// Y2
		for (int i = 0; i < 16; ++i) {
			int16_t c = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
			c = (i == 0) ? (int16_t)((c >= 0 ? (c + qf->y2_dc / 2) : (c - qf->y2_dc / 2)) / qf->y2_dc)
			            : (int16_t)((c >= 0 ? (c + qf->y2_ac / 2) : (c - qf->y2_ac / 2)) / qf->y2_ac);
			p[0] = (uint8_t)((uint16_t)c & 0xFFu);
			p[1] = (uint8_t)(((uint16_t)c >> 8) & 0xFFu);
			p += 2;
		}

		// Y blocks
		for (int b = 0; b < 16; ++b) {
			for (int i = 0; i < 16; ++i) {
				int16_t c = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
				c = (i == 0) ? (int16_t)((c >= 0 ? (c + qf->y1_dc / 2) : (c - qf->y1_dc / 2)) / qf->y1_dc)
				            : (int16_t)((c >= 0 ? (c + qf->y1_ac / 2) : (c - qf->y1_ac / 2)) / qf->y1_ac);
				p[0] = (uint8_t)((uint16_t)c & 0xFFu);
				p[1] = (uint8_t)(((uint16_t)c >> 8) & 0xFFu);
				p += 2;
			}
		}

		// U blocks
		for (int b = 0; b < 4; ++b) {
			for (int i = 0; i < 16; ++i) {
				int16_t c = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
				c = (i == 0) ? (int16_t)((c >= 0 ? (c + qf->uv_dc / 2) : (c - qf->uv_dc / 2)) / qf->uv_dc)
				            : (int16_t)((c >= 0 ? (c + qf->uv_ac / 2) : (c - qf->uv_ac / 2)) / qf->uv_ac);
				p[0] = (uint8_t)((uint16_t)c & 0xFFu);
				p[1] = (uint8_t)(((uint16_t)c >> 8) & 0xFFu);
				p += 2;
			}
		}

		// V blocks
		for (int b = 0; b < 4; ++b) {
			for (int i = 0; i < 16; ++i) {
				int16_t c = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
				c = (i == 0) ? (int16_t)((c >= 0 ? (c + qf->uv_dc / 2) : (c - qf->uv_dc / 2)) / qf->uv_dc)
				            : (int16_t)((c >= 0 ? (c + qf->uv_ac / 2) : (c - qf->uv_ac / 2)) / qf->uv_ac);
				p[0] = (uint8_t)((uint16_t)c & 0xFFu);
				p[1] = (uint8_t)(((uint16_t)c >> 8) & 0xFFu);
				p += 2;
			}
		}
	}
}

int main(int argc, char** argv) {
	int quality = 75;
	int argi = 1;
	if (argi + 1 < argc && strcmp(argv[argi], "--q") == 0) {
		if (parse_int(argv[argi + 1], &quality) != 0) {
			usage(argv[0]);
			return 2;
		}
		argi += 2;
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

	const int qindex = enc_vp8_qindex_from_quality_libwebp(quality);
	EncVp8QuantFactors qf;
	enc_vp8_quant_factors_from_qindex(qindex,
	                                 /*y1_dc_delta=*/0,
	                                 /*y2_dc_delta=*/0,
	                                 /*y2_ac_delta=*/0,
	                                 /*uv_dc_delta=*/0,
	                                 /*uv_ac_delta=*/0,
	                                 &qf);

	quantize_dump_inplace(dump, dump_size, &qf);

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
	        "%s: q=%d -> qindex=%d (y1_dc=%d y1_ac=%d y2_dc=%d y2_ac=%d uv_dc=%d uv_ac=%d) -> %zu bytes\n",
	        in_path,
	        quality,
	        qf.qindex,
	        qf.y1_dc,
	        qf.y1_ac,
	        qf.y2_dc,
	        qf.y2_ac,
	        qf.uv_dc,
	        qf.uv_ac,
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
