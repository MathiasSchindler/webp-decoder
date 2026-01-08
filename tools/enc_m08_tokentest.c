#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/common/os.h"
#include "../src/enc-m00_png/enc_png.h"
#include "../src/enc-m04_yuv/enc_rgb_to_yuv.h"
#include "../src/enc-m05_intra/enc_intra_dc.h"
#include "../src/enc-m06_quant/enc_quant.h"
#include "../src/enc-m07_tokens/enc_vp8_tokens.h"
#include "../src/m05_tokens/vp8_tokens.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--q <0..100>] <in.png> [out.vp8|-]\n"
	        "\n"
	        "Builds a VP8 keyframe payload with DC_PRED and encoded coeff tokens,\n"
	        "and validates that the decoder parses identical coefficient values.\n"
	        "Writes VP8 payload bytes to out.vp8 or stdout.\n",
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

static uint64_t fnv1a64_init(void) { return 1469598103934665603ull; }
static uint64_t fnv1a64_u32(uint64_t h, uint32_t v) {
	h ^= (uint64_t)(v & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 8) & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 16) & 0xffu);
	h *= 1099511628211ull;
	h ^= (uint64_t)((v >> 24) & 0xffu);
	h *= 1099511628211ull;
	return h;
}
static uint64_t fnv1a64_i32(uint64_t h, int32_t v) { return fnv1a64_u32(h, (uint32_t)v); }

static int16_t load_i16le(const uint8_t* p) {
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void store_i16le(uint8_t* p, int16_t v) {
	uint16_t u = (uint16_t)v;
	p[0] = (uint8_t)(u & 0xFFu);
	p[1] = (uint8_t)((u >> 8) & 0xFFu);
}

static void quantize_dump_bytes_inplace(uint8_t* dump, size_t dump_size, const EncVp8QuantFactors* qf) {
	// Layout per macroblock, in int16 LE:
	// Y2(16), Y(16*16), U(4*16), V(4*16)
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t bytes_per_mb = coeffs_per_mb * 2;
	if (!dump || dump_size % bytes_per_mb != 0) return;
	const size_t mb_total = dump_size / bytes_per_mb;

	for (size_t mb = 0; mb < mb_total; ++mb) {
		uint8_t* p = dump + mb * bytes_per_mb;

		int16_t blk[16];

		// Y2
		for (int i = 0; i < 16; ++i) blk[i] = load_i16le(p + (size_t)i * 2);
		enc_vp8_quantize4x4_inplace(blk, qf->y2_dc, qf->y2_ac);
		for (int i = 0; i < 16; ++i) store_i16le(p + (size_t)i * 2, blk[i]);
		p += 16 * 2;

		// Y
		for (int b = 0; b < 16; ++b) {
			for (int i = 0; i < 16; ++i) blk[i] = load_i16le(p + (size_t)i * 2);
			enc_vp8_quantize4x4_inplace(blk, qf->y1_dc, qf->y1_ac);
			for (int i = 0; i < 16; ++i) store_i16le(p + (size_t)i * 2, blk[i]);
			p += 16 * 2;
		}

		// U
		for (int b = 0; b < 4; ++b) {
			for (int i = 0; i < 16; ++i) blk[i] = load_i16le(p + (size_t)i * 2);
			enc_vp8_quantize4x4_inplace(blk, qf->uv_dc, qf->uv_ac);
			for (int i = 0; i < 16; ++i) store_i16le(p + (size_t)i * 2, blk[i]);
			p += 16 * 2;
		}

		// V
		for (int b = 0; b < 4; ++b) {
			for (int i = 0; i < 16; ++i) blk[i] = load_i16le(p + (size_t)i * 2);
			enc_vp8_quantize4x4_inplace(blk, qf->uv_dc, qf->uv_ac);
			for (int i = 0; i < 16; ++i) store_i16le(p + (size_t)i * 2, blk[i]);
			p += 16 * 2;
		}
	}
}

static int dump_bytes_to_i16(const uint8_t* dump, size_t dump_size, int16_t** out, size_t* out_count) {
	if (!out || !out_count) return -1;
	*out = NULL;
	*out_count = 0;
	if (!dump || (dump_size % 2) != 0) return -1;
	const size_t count = dump_size / 2;
	if (count > (SIZE_MAX / sizeof(int16_t))) return -1;
	int16_t* a = (int16_t*)malloc(count * sizeof(int16_t));
	if (!a) return -1;
	for (size_t i = 0; i < count; ++i) {
		a[i] = load_i16le(dump + i * 2);
	}
	*out = a;
	*out_count = count;
	return 0;
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
	enc_vp8_quant_factors_from_qindex(qindex, 0, 0, 0, 0, 0, &qf);
	quantize_dump_bytes_inplace(dump, dump_size, &qf);

	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	if (dump_bytes_to_i16(dump, dump_size, &coeffs, &coeffs_count) != 0) {
		fprintf(stderr, "%s: dump_bytes_to_i16 failed\n", in_path);
		free(dump);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enc_vp8_build_keyframe_dc_coeffs(img.width,
	                                    img.height,
	                                    (uint8_t)qindex,
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
		free(dump);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	Vp8CoeffStats stats;
	if (vp8_decode_coeff_stats((ByteSpan){.data = vp8, .size = vp8_size}, &stats) != 0) {
		fprintf(stderr, "%s: vp8_decode_coeff_stats failed (errno=%d)\n", in_path, errno);
		free(vp8);
		free(coeffs);
		free(dump);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	uint64_t expected = fnv1a64_init();
	for (size_t i = 0; i < coeffs_count; ++i) {
		expected = fnv1a64_i32(expected, (int32_t)coeffs[i]);
	}

	if (expected != stats.coeff_hash_fnv1a64) {
		fprintf(stderr,
		        "%s: M8 token mismatch q=%d qindex=%d expected_hash=%016" PRIx64
		        " decoded_hash=%016" PRIx64 "\n",
		        in_path,
		        quality,
		        qindex,
		        expected,
		        stats.coeff_hash_fnv1a64);
		free(vp8);
		free(coeffs);
		free(dump);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	FILE* out = stdout;
	if (strcmp(out_path, "-") != 0) {
		out = fopen(out_path, "wb");
		if (!out) {
			fprintf(stderr, "fopen(%s) failed (errno=%d)\n", out_path, errno);
			free(vp8);
			free(coeffs);
			free(dump);
			enc_yuv420_free(&yuv);
			enc_png_free(&img);
			return 1;
		}
	}

	fprintf(stderr,
	        "%s: q=%d qindex=%d -> vp8=%zu bytes (coeff_hash=%016" PRIx64 ")\n",
	        in_path,
	        quality,
	        qindex,
	        vp8_size,
	        expected);

	int ok = fwrite_all(out, vp8, vp8_size);
	if (out != stdout) fclose(out);

	free(vp8);
	free(coeffs);
	free(dump);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);

	if (ok != 0) {
		fprintf(stderr, "write failed\n");
		return 1;
	}

	return 0;
}
