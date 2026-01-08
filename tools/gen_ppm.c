#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t lcg_next(uint32_t* s) {
	*s = (*s) * 1664525u + 1013904223u;
	return *s;
}

static void write_header(FILE* f, int w, int h) {
	fprintf(f, "P6\n%d %d\n255\n", w, h);
}

static int write_pixel(FILE* f, uint8_t r, uint8_t g, uint8_t b) {
	uint8_t p[3] = {r, g, b};
	return fwrite(p, 1, 3, f) == 3 ? 0 : -1;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s <pattern> <w> <h> <out.ppm> [seed]\n", argv[0]);
		fprintf(stderr, "patterns: solid,rgbgrad,checker,noise,diag\n");
		return 2;
	}
	const char* pattern = argv[1];
	int w = atoi(argv[2]);
	int h = atoi(argv[3]);
	const char* out = argv[4];
	uint32_t seed = (argc >= 6) ? (uint32_t)strtoul(argv[5], NULL, 0) : 1u;
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
		errno = EINVAL;
		perror("bad size");
		return 2;
	}

	FILE* f = fopen(out, "wb");
	if (!f) {
		perror("fopen");
		return 1;
	}
	write_header(f, w, h);

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t r = 0, g = 0, b = 0;
			if (strcmp(pattern, "solid") == 0) {
				r = 17;
				g = 34;
				b = 51;
			} else if (strcmp(pattern, "rgbgrad") == 0) {
				r = (uint8_t)((x * 255) / (w - 1));
				g = (uint8_t)((y * 255) / (h - 1));
				b = (uint8_t)(((x + y) * 255) / (w + h - 2));
			} else if (strcmp(pattern, "checker") == 0) {
				int s = 8;
				int v = (((x / s) ^ (y / s)) & 1) ? 255 : 0;
				r = g = b = (uint8_t)v;
			} else if (strcmp(pattern, "noise") == 0) {
				uint32_t v = lcg_next(&seed);
				r = (uint8_t)(v & 0xff);
				g = (uint8_t)((v >> 8) & 0xff);
				b = (uint8_t)((v >> 16) & 0xff);
			} else if (strcmp(pattern, "diag") == 0) {
				int v = (abs(x - y) <= 1) ? 255 : 0;
				r = g = b = (uint8_t)v;
			} else {
				fprintf(stderr, "unknown pattern: %s\n", pattern);
				fclose(f);
				return 2;
			}

			if (write_pixel(f, r, g, b) != 0) {
				fprintf(stderr, "write failed\n");
				fclose(f);
				return 1;
			}
		}
	}

	if (fclose(f) != 0) {
		perror("fclose");
		return 1;
	}
	return 0;
}
