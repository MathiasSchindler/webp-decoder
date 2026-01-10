#include "quality_ppm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_ws(int c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int read_token(FILE* f, char* buf, size_t buf_len) {
	if (buf_len == 0) return -1;

	int c;
	for (;;) {
		c = fgetc(f);
		if (c == EOF) return -1;
		if (is_ws(c)) continue;
		if (c == '#') {
			while ((c = fgetc(f)) != EOF && c != '\n') {
			}
			continue;
		}
		break;
	}

	size_t i = 0;
	while (c != EOF && !is_ws(c) && c != '#') {
		if (i + 1 >= buf_len) return -1;
		buf[i++] = (char)c;
		c = fgetc(f);
	}
	buf[i] = '\0';

	if (c != EOF && is_ws(c)) {
		ungetc(c, f);
	} else if (c == '#') {
		while ((c = fgetc(f)) != EOF && c != '\n') {
		}
	}
	return (i > 0) ? 0 : -1;
}

int quality_ppm_read_stream(FILE* f, QualityPpmImage* out_img) {
	if (!out_img) return -1;
	memset(out_img, 0, sizeof(*out_img));
	if (!f) return -1;

	char tok[64];
	if (read_token(f, tok, sizeof(tok)) != 0 || strcmp(tok, "P6") != 0) {
		errno = EINVAL;
		return -1;
	}

	if (read_token(f, tok, sizeof(tok)) != 0) {
		errno = EINVAL;
		return -1;
	}
	char* end = NULL;
	unsigned long w = strtoul(tok, &end, 10);
	if (!end || *end != '\0' || w == 0 || w > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}

	if (read_token(f, tok, sizeof(tok)) != 0) {
		errno = EINVAL;
		return -1;
	}
	unsigned long h = strtoul(tok, &end, 10);
	if (!end || *end != '\0' || h == 0 || h > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}

	if (read_token(f, tok, sizeof(tok)) != 0) {
		errno = EINVAL;
		return -1;
	}
	unsigned long maxv = strtoul(tok, &end, 10);
	if (!end || *end != '\0' || maxv != 255) {
		errno = EINVAL;
		return -1;
	}

	/*
	PPM requires a single whitespace separator after maxval. Do not skip arbitrary
	"whitespace" here: pixel bytes may legitimately be 0x0a, 0x20, etc.
	*/
	int c = fgetc(f);
	if (c == EOF || !is_ws(c)) {
		errno = EINVAL;
		return -1;
	}

	uint64_t npx = (uint64_t)(uint32_t)w * (uint64_t)(uint32_t)h;
	uint64_t nbytes64 = npx * 3u;
	if (nbytes64 > (uint64_t)SIZE_MAX) {
		errno = ENOMEM;
		return -1;
	}
	size_t nbytes = (size_t)nbytes64;

	uint8_t* rgb = (uint8_t*)malloc(nbytes);
	if (!rgb) {
		return -1;
	}

	if (fread(rgb, 1, nbytes, f) != nbytes) {
		free(rgb);
		errno = EINVAL;
		return -1;
	}

	out_img->width = (uint32_t)w;
	out_img->height = (uint32_t)h;
	out_img->rgb = rgb;
	return 0;
}

int quality_ppm_read_file(const char* path, QualityPpmImage* out_img) {
	if (!out_img) return -1;
	memset(out_img, 0, sizeof(*out_img));

	FILE* f = fopen(path, "rb");
	if (!f) return -1;

	int rc = quality_ppm_read_stream(f, out_img);
	fclose(f);
	return rc;
}

void quality_ppm_free(QualityPpmImage* img) {
	if (!img) return;
	free(img->rgb);
	memset(img, 0, sizeof(*img));
}