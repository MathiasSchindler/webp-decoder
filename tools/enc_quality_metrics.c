#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/quality/quality_ppm.h"
#include "../src/quality/quality_psnr.h"
#include "../src/quality/quality_ssim.h"

static void usage(const char* argv0) {
	fprintf(stderr, "Usage: %s <ref.ppm> <dist.ppm|->\n", argv0);
}

static void print_val(const char* key, double v) {
	if (isinf(v)) {
		printf("%s=inf", key);
	} else {
		printf("%s=%.6f", key, v);
	}
}

int main(int argc, char** argv) {
	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	const char* ref_path = argv[1];
	const char* dist_path = argv[2];

	QualityPpmImage a;
	QualityPpmImage b;
	if (quality_ppm_read_file(ref_path, &a) != 0) {
		fprintf(stderr, "quality_ppm_read_file failed: %s (errno=%d)\n", ref_path, errno);
		return 1;
	}
	int dist_ok = 0;
	if (strcmp(dist_path, "-") == 0) {
		dist_ok = (quality_ppm_read_stream(stdin, &b) == 0);
	} else {
		dist_ok = (quality_ppm_read_file(dist_path, &b) == 0);
	}
	if (!dist_ok) {
		fprintf(stderr, "quality_ppm_read_file failed: %s (errno=%d)\n", dist_path, errno);
		quality_ppm_free(&a);
		return 1;
	}

	if (a.width != b.width || a.height != b.height) {
		fprintf(stderr, "size mismatch: %ux%u vs %ux%u\n", a.width, a.height, b.width,
		        b.height);
		quality_ppm_free(&a);
		quality_ppm_free(&b);
		return 1;
	}

	QualityPsnr psnr;
	if (quality_psnr_rgb24(a.rgb, b.rgb, a.width, a.height, &psnr) != 0) {
		fprintf(stderr, "quality_psnr_rgb24 failed\n");
		quality_ppm_free(&a);
		quality_ppm_free(&b);
		return 1;
	}

	QualitySsim ssim;
	if (quality_ssim_y_from_rgb24(a.rgb, b.rgb, a.width, a.height, &ssim) != 0) {
		fprintf(stderr, "quality_ssim_y_from_rgb24 failed\n");
		quality_ppm_free(&a);
		quality_ppm_free(&b);
		return 1;
	}

	print_val("psnr_rgb", psnr.psnr_rgb);
	printf(" ");
	print_val("psnr_r", psnr.psnr_r);
	printf(" ");
	print_val("psnr_g", psnr.psnr_g);
	printf(" ");
	print_val("psnr_b", psnr.psnr_b);
	printf(" ");
	print_val("ssim_y", ssim.ssim_y);
	printf("\n");

	quality_ppm_free(&a);
	quality_ppm_free(&b);
	return 0;
}