#include "enc_riff.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void le32_store(uint8_t out[4], uint32_t v) {
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)((v >> 8) & 0xFFu);
	out[2] = (uint8_t)((v >> 16) & 0xFFu);
	out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int write_all(int fd, const void* data, size_t n) {
	const uint8_t* p = (const uint8_t*)data;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) return -1;
		if (w == 0) {
			errno = EIO;
			return -1;
		}
		p += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

int enc_webp_write_vp8_file(const char* out_path, const uint8_t* vp8_payload, size_t vp8_size) {
	if (!out_path) return -1;
	if (vp8_size && !vp8_payload) return -1;

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;

	uint8_t hdr[12];
	memcpy(hdr + 0, "RIFF", 4);
	// RIFF size field is file_size - 8
	const uint32_t pad = (uint32_t)(vp8_size & 1u);
	const uint64_t file_size = 12ull + 8ull + (uint64_t)vp8_size + (uint64_t)pad;
	if (file_size > 0xFFFFFFFFull) {
		(void)close(fd);
		errno = EOVERFLOW;
		return -1;
	}
	le32_store(hdr + 4, (uint32_t)(file_size - 8ull));
	memcpy(hdr + 8, "WEBP", 4);

	uint8_t chdr[8];
	memcpy(chdr + 0, "VP8 ", 4);
	le32_store(chdr + 4, (uint32_t)vp8_size);

	int ok = 0;
	if (write_all(fd, hdr, sizeof(hdr)) != 0) ok = -1;
	if (ok == 0 && write_all(fd, chdr, sizeof(chdr)) != 0) ok = -1;
	if (ok == 0 && vp8_size && write_all(fd, vp8_payload, vp8_size) != 0) ok = -1;
	if (ok == 0 && pad) {
		uint8_t z = 0;
		if (write_all(fd, &z, 1) != 0) ok = -1;
	}
	if (close(fd) != 0) ok = -1;
	return ok;
}
