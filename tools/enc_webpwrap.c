#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/enc-m01_riff/enc_riff.h"

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage:\n"
	        "  %s <in.webp> <out.webp>\n"
	        "\n"
	        "Extracts the first VP8 chunk payload from <in.webp> and wraps it\n"
	        "into a new minimal RIFF/WEBP container written to <out.webp>.\n",
	        argv0);
}

static uint32_t le32_load(const uint8_t p[4]) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_entire_file(const char* path, uint8_t** out_buf, size_t* out_size) {
	*out_buf = NULL;
	*out_size = 0;

	FILE* f = fopen(path, "rb");
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		return -1;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}

	uint8_t* buf = (uint8_t*)malloc((size_t)sz);
	if (!buf) {
		fclose(f);
		errno = ENOMEM;
		return -1;
	}

	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (n != (size_t)sz) {
		free(buf);
		return -1;
	}

	*out_buf = buf;
	*out_size = n;
	return 0;
}

static int extract_vp8_payload(const uint8_t* file, size_t file_size, const uint8_t** out_vp8, size_t* out_vp8_size) {
	*out_vp8 = NULL;
	*out_vp8_size = 0;

	if (file_size < 12) return -1;
	if (memcmp(file + 0, "RIFF", 4) != 0) return -1;
	if (memcmp(file + 8, "WEBP", 4) != 0) return -1;

	size_t off = 12;
	while (off + 8 <= file_size) {
		const uint8_t* ch = file + off;
		uint32_t clen = le32_load(ch + 4);
		off += 8;
		if (off + clen > file_size) return -1;
		if (memcmp(ch, "VP8 ", 4) == 0) {
			*out_vp8 = file + off;
			*out_vp8_size = (size_t)clen;
			return 0;
		}
		off += clen;
		if (clen & 1u) {
			if (off >= file_size) return -1;
			off++;
		}
	}

	return -1;
}

static int validate_riff_sizes(const char* path) {
	struct stat st;
	if (stat(path, &st) != 0) return -1;
	if (st.st_size < 12) return -1;

	FILE* f = fopen(path, "rb");
	if (!f) return -1;
	uint8_t hdr[12];
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	if (memcmp(hdr + 0, "RIFF", 4) != 0) return -1;
	if (memcmp(hdr + 8, "WEBP", 4) != 0) return -1;
	uint32_t riff_size = le32_load(hdr + 4);
	uint64_t expect = (uint64_t)st.st_size - 8ull;
	if (expect > 0xFFFFFFFFull) return -1;
	return (riff_size == (uint32_t)expect) ? 0 : -1;
}

int main(int argc, char** argv) {
	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	const char* in_path = argv[1];
	const char* out_path = argv[2];

	uint8_t* file = NULL;
	size_t file_size = 0;
	if (read_entire_file(in_path, &file, &file_size) != 0) {
		fprintf(stderr, "read %s failed (errno=%d)\n", in_path, errno);
		return 1;
	}

	const uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (extract_vp8_payload(file, file_size, &vp8, &vp8_size) != 0) {
		fprintf(stderr, "%s: failed to find VP8 chunk\n", in_path);
		free(file);
		return 1;
	}

	if (enc_webp_write_vp8_file(out_path, vp8, vp8_size) != 0) {
		fprintf(stderr, "write %s failed (errno=%d)\n", out_path, errno);
		free(file);
		return 1;
	}
	free(file);

	if (validate_riff_sizes(out_path) != 0) {
		fprintf(stderr, "%s: RIFF size field mismatch\n", out_path);
		return 1;
	}

	return 0;
}
