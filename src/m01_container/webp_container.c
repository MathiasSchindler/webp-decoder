#include "webp_container.h"

#include <errno.h>

static int need(size_t off, size_t n, size_t size) {
	return (off <= size) && (n <= size - off);
}

static uint32_t load_u32_le(const uint8_t* p) {
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int fourcc_eq(uint32_t le, const char a[4]) {
	return (uint8_t)le == (uint8_t)a[0] && (uint8_t)(le >> 8) == (uint8_t)a[1] &&
	       (uint8_t)(le >> 16) == (uint8_t)a[2] && (uint8_t)(le >> 24) == (uint8_t)a[3];
}

int webp_parse_simple_lossy(ByteSpan file, WebPContainer* out) {
	if (!out) return -1;
	out->riff_size = 0;
	out->actual_size = file.size;
	out->vp8_chunk_offset = 0;
	out->vp8_chunk_size = 0;

	// Need RIFF header: 'RIFF' + size + 'WEBP'
	if (!file.data || file.size < 12) {
		errno = EINVAL;
		return -1;
	}

	size_t off = 0;
	uint32_t riff = load_u32_le(file.data + off);
	if (!fourcc_eq(riff, "RIFF")) {
		errno = EINVAL;
		return -1;
	}
	off += 4;

	uint32_t riff_size = load_u32_le(file.data + off);
	out->riff_size = riff_size;
	off += 4;

	uint32_t webp = load_u32_le(file.data + off);
	if (!fourcc_eq(webp, "WEBP")) {
		errno = EINVAL;
		return -1;
	}
	off += 4;

	// Strict check for now: file size must match header.
	// RIFF size counts from offset 8, includes 'WEBP' FourCC.
	size_t expected_total = (size_t)riff_size + 8;
	if (expected_total != file.size) {
		errno = EINVAL;
		return -1;
	}

	// Parse exactly one chunk: 'VP8 ' + size + payload (+ pad to even)
	if (!need(off, 8, file.size)) {
		errno = EINVAL;
		return -1;
	}
	uint32_t chunk_tag = load_u32_le(file.data + off);
	off += 4;
	uint32_t chunk_size = load_u32_le(file.data + off);
	off += 4;

	if (!fourcc_eq(chunk_tag, "VP8 ")) {
		errno = EINVAL;
		return -1;
	}
	if (!need(off, chunk_size, file.size)) {
		errno = EINVAL;
		return -1;
	}
	out->vp8_chunk_offset = off;
	out->vp8_chunk_size = chunk_size;
	off += chunk_size;
	if (off & 1u) off++; // padding

	// No extra chunks allowed in milestone 1.
	if (off != file.size) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}
