#include "enc_riff.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void le32_store(uint8_t out[4], uint32_t v) {
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)((v >> 8) & 0xFFu);
	out[2] = (uint8_t)((v >> 16) & 0xFFu);
	out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int fwrite_all(FILE* f, const void* data, size_t n) {
	return fwrite(data, 1, n, f) == n ? 0 : -1;
}

int enc_webp_write_vp8_file(const char* out_path, const uint8_t* vp8_payload, size_t vp8_size) {
	if (!out_path) return -1;
	if (vp8_size && !vp8_payload) return -1;

	FILE* out = fopen(out_path, "wb");
	if (!out) return -1;

	uint8_t hdr[12];
	memcpy(hdr + 0, "RIFF", 4);
	// RIFF size field is file_size - 8
	const uint32_t pad = (uint32_t)(vp8_size & 1u);
	const uint64_t file_size = 12ull + 8ull + (uint64_t)vp8_size + (uint64_t)pad;
	if (file_size > 0xFFFFFFFFull) {
		fclose(out);
		errno = EOVERFLOW;
		return -1;
	}
	le32_store(hdr + 4, (uint32_t)(file_size - 8ull));
	memcpy(hdr + 8, "WEBP", 4);

	uint8_t chdr[8];
	memcpy(chdr + 0, "VP8 ", 4);
	le32_store(chdr + 4, (uint32_t)vp8_size);

	int ok = 0;
	if (fwrite_all(out, hdr, sizeof(hdr)) != 0) ok = -1;
	if (ok == 0 && fwrite_all(out, chdr, sizeof(chdr)) != 0) ok = -1;
	if (ok == 0 && vp8_size && fwrite_all(out, vp8_payload, vp8_size) != 0) ok = -1;
	if (ok == 0 && pad) {
		uint8_t z = 0;
		if (fwrite_all(out, &z, 1) != 0) ok = -1;
	}

	if (fclose(out) != 0) ok = -1;
	return ok;
}
