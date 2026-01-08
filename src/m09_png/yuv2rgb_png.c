#include "yuv2rgb_png.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../common/os.h"

enum {
	YUV_FIX2 = 6,
	YUV_MASK2 = (256 << YUV_FIX2) - 1
};

static inline int mult_hi(int v, int coeff) {
	return (v * coeff) >> 8;
}

static inline uint8_t vp8_clip8(int v) {
	if ((v & ~YUV_MASK2) == 0) return (uint8_t)(v >> YUV_FIX2);
	return (v < 0) ? 0u : 255u;
}

static inline void vp8_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* dst3) {
	const int Y = (int)y;
	const int U = (int)u;
	const int V = (int)v;
	const int r = mult_hi(Y, 19077) + mult_hi(V, 26149) - 14234;
	const int g = mult_hi(Y, 19077) - mult_hi(U, 6419) - mult_hi(V, 13320) + 8708;
	const int b = mult_hi(Y, 19077) + mult_hi(U, 33050) - 17685;
	dst3[0] = vp8_clip8(r);
	dst3[1] = vp8_clip8(g);
	dst3[2] = vp8_clip8(b);
}

// Matches libwebp's DSP fancy upsampler (see libwebp src/dsp/upsampling.c).
static void upsample_rgb_line_pair(const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u,
                                   const uint8_t* top_v, const uint8_t* cur_u, const uint8_t* cur_v,
                                   uint8_t* top_dst, uint8_t* bottom_dst, uint32_t len) {
	if (len == 0) return;

	const uint32_t last_pixel_pair = (len - 1u) >> 1;
	uint32_t tl_u = top_u[0];
	uint32_t tl_v = top_v[0];
	uint32_t l_u = cur_u[0];
	uint32_t l_v = cur_v[0];

	{
		const uint8_t u0 = (uint8_t)((3u * tl_u + l_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * tl_v + l_v + 2u) >> 2);
		vp8_yuv_to_rgb(top_y[0], u0, v0, top_dst + 0);
	}
	if (bottom_y != NULL) {
		const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
		vp8_yuv_to_rgb(bottom_y[0], u0, v0, bottom_dst + 0);
	}

	for (uint32_t x = 1; x <= last_pixel_pair; ++x) {
		const uint32_t t_u = top_u[x];
		const uint32_t t_v = top_v[x];
		const uint32_t u = cur_u[x];
		const uint32_t v = cur_v[x];

		const uint32_t avg_u = tl_u + t_u + l_u + u + 8u;
		const uint32_t avg_v = tl_v + t_v + l_v + v + 8u;
		const uint32_t diag_12_u = (avg_u + 2u * (t_u + l_u)) >> 3;
		const uint32_t diag_12_v = (avg_v + 2u * (t_v + l_v)) >> 3;
		const uint32_t diag_03_u = (avg_u + 2u * (tl_u + u)) >> 3;
		const uint32_t diag_03_v = (avg_v + 2u * (tl_v + v)) >> 3;

		{
			const uint8_t u0 = (uint8_t)((diag_12_u + tl_u) >> 1);
			const uint8_t v0 = (uint8_t)((diag_12_v + tl_v) >> 1);
			const uint8_t u1 = (uint8_t)((diag_03_u + t_u) >> 1);
			const uint8_t v1 = (uint8_t)((diag_03_v + t_v) >> 1);
			vp8_yuv_to_rgb(top_y[2u * x - 1u], u0, v0, top_dst + (2u * x - 1u) * 3u);
			vp8_yuv_to_rgb(top_y[2u * x + 0u], u1, v1, top_dst + (2u * x + 0u) * 3u);
		}
		if (bottom_y != NULL) {
			const uint8_t u0 = (uint8_t)((diag_03_u + l_u) >> 1);
			const uint8_t v0 = (uint8_t)((diag_03_v + l_v) >> 1);
			const uint8_t u1 = (uint8_t)((diag_12_u + u) >> 1);
			const uint8_t v1 = (uint8_t)((diag_12_v + v) >> 1);
			vp8_yuv_to_rgb(bottom_y[2u * x - 1u], u0, v0, bottom_dst + (2u * x - 1u) * 3u);
			vp8_yuv_to_rgb(bottom_y[2u * x + 0u], u1, v1, bottom_dst + (2u * x + 0u) * 3u);
		}

		tl_u = t_u;
		tl_v = t_v;
		l_u = u;
		l_v = v;
	}

	if ((len & 1u) == 0u) {
		const uint32_t idx = len - 1u;
		{
			const uint8_t u0 = (uint8_t)((3u * tl_u + l_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * tl_v + l_v + 2u) >> 2);
			vp8_yuv_to_rgb(top_y[idx], u0, v0, top_dst + idx * 3u);
		}
		if (bottom_y != NULL) {
			const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
			vp8_yuv_to_rgb(bottom_y[idx], u0, v0, bottom_dst + idx * 3u);
		}
	}
}

static inline uint32_t be32(uint32_t x) {
	return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) | ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
	static uint32_t table[256];
	static int table_init = 0;
	if (!table_init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		table_init = 1;
	}
	crc ^= 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc = table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

static int write_chunk(int fd, const char type[4], const uint8_t* data, uint32_t len) {
	uint8_t hdr[8];
	uint32_t len_be = be32(len);
	memcpy(hdr + 0, &len_be, 4);
	memcpy(hdr + 4, type, 4);
	if (os_write_all(fd, hdr, sizeof(hdr)) != 0) return -1;
	if (len != 0 && os_write_all(fd, data, len) != 0) return -1;
	uint32_t crc = 0;
	crc = crc32_update(crc, (const uint8_t*)type, 4);
	if (len != 0) crc = crc32_update(crc, data, len);
	uint32_t crc_be = be32(crc);
	if (os_write_all(fd, &crc_be, 4) != 0) return -1;
	return 0;
}

static inline void adler32_update(uint32_t* a, uint32_t* b, const uint8_t* buf, size_t len) {
	// Adler-32 modulo.
	const uint32_t MOD = 65521u;
	uint32_t aa = *a;
	uint32_t bb = *b;
	for (size_t i = 0; i < len; i++) {
		aa += buf[i];
		if (aa >= MOD) aa -= MOD;
		bb += aa;
		bb %= MOD;
	}
	*a = aa;
	*b = bb;
}

typedef struct {
	const Yuv420Image* img;
	uint8_t* top_row;
	uint8_t* bottom_row;
	uint8_t* scanline;
	uint32_t row_bytes;
	uint32_t scanline_bytes;
	uint32_t y;
	int have_cached_bottom;
} PngRgbGen;

static int png_fill_scanline(PngRgbGen* g) {
	g->scanline[0] = 0; // filter type 0
	if (g->y == 0) {
		const uint8_t* y0 = g->img->y;
		const uint8_t* u0 = g->img->u;
		const uint8_t* v0 = g->img->v;
		upsample_rgb_line_pair(y0, NULL, u0, v0, u0, v0, g->top_row, NULL, g->img->width);
		memcpy(g->scanline + 1, g->top_row, g->row_bytes);
		return 0;
	}
	if (g->have_cached_bottom) {
		memcpy(g->scanline + 1, g->bottom_row, g->row_bytes);
		g->have_cached_bottom = 0;
		return 0;
	}

	const uint8_t* top_y_ptr = g->img->y + (size_t)g->y * g->img->stride_y;
	const uint8_t* bottom_y_ptr = (g->y + 1u < g->img->height)
	                               ? (g->img->y + (size_t)(g->y + 1u) * g->img->stride_y)
	                               : NULL;
	const uint32_t ch = (g->img->height + 1u) >> 1;
	const uint32_t top_cy = g->y >> 1;
	const uint32_t cur_cy = (top_cy + 1u < ch) ? (top_cy + 1u) : (ch - 1u);
	const uint8_t* top_u = g->img->u + (size_t)top_cy * g->img->stride_uv;
	const uint8_t* top_v = g->img->v + (size_t)top_cy * g->img->stride_uv;
	const uint8_t* cur_u = g->img->u + (size_t)cur_cy * g->img->stride_uv;
	const uint8_t* cur_v = g->img->v + (size_t)cur_cy * g->img->stride_uv;

	upsample_rgb_line_pair(top_y_ptr, bottom_y_ptr, top_u, top_v, cur_u, cur_v, g->top_row, g->bottom_row, g->img->width);
	memcpy(g->scanline + 1, g->top_row, g->row_bytes);
	if (bottom_y_ptr != NULL) g->have_cached_bottom = 1;
	return 0;
}

int yuv420_write_png_fd(int fd, const Yuv420Image* img) {
	if (fd < 0 || !img || !img->y || !img->u || !img->v) {
		errno = EINVAL;
		return -1;
	}
	if (img->width == 0 || img->height == 0) {
		errno = EINVAL;
		return -1;
	}

	// PNG signature.
	static const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
	if (os_write_all(fd, sig, sizeof(sig)) != 0) return -1;

	// IHDR.
	uint8_t ihdr[13];
	uint32_t w_be = be32(img->width);
	uint32_t h_be = be32(img->height);
	memcpy(ihdr + 0, &w_be, 4);
	memcpy(ihdr + 4, &h_be, 4);
	ihdr[8] = 8;  // bit depth
	ihdr[9] = 2;  // color type: truecolor (RGB)
	ihdr[10] = 0; // compression
	ihdr[11] = 0; // filter
	ihdr[12] = 0; // interlace
	if (write_chunk(fd, "IHDR", ihdr, sizeof(ihdr)) != 0) return -1;

	// Build the zlib stream into memory (raw scanlines are generated on the fly).
	const uint32_t row_bytes = img->width * 3u;
	const uint32_t scanline_bytes = 1u + row_bytes; // filter byte + RGB
	const uint64_t raw_size64 = (uint64_t)img->height * (uint64_t)(1u + row_bytes);
	if (raw_size64 > 0x7FFFFFFFu) {
		errno = EFBIG;
		return -1;
	}
	const uint32_t raw_size = (uint32_t)raw_size64;
	const uint32_t blocks = (raw_size + 65535u - 1u) / 65535u;
	const uint64_t zsize64 = 2u + (uint64_t)raw_size + (uint64_t)blocks * 5u + 4u;
	if (zsize64 > SIZE_MAX) {
		errno = ENOMEM;
		return -1;
	}
	uint8_t* z = (uint8_t*)malloc((size_t)zsize64);
	if (!z) {
		errno = ENOMEM;
		return -1;
	}

	// zlib header: 0x78 0x01 (no compression / fastest).
	size_t zp = 0;
	z[zp++] = 0x78u;
	z[zp++] = 0x01u;

	uint32_t ad_a = 1u;
	uint32_t ad_b = 0u;

	uint8_t* top_row = (uint8_t*)malloc((size_t)row_bytes);
	uint8_t* bottom_row = (uint8_t*)malloc((size_t)row_bytes);
	uint8_t* scanline = (uint8_t*)malloc((size_t)scanline_bytes);
	if (!top_row || !bottom_row || !scanline) {
		free(top_row);
		free(bottom_row);
		free(scanline);
		free(z);
		errno = ENOMEM;
		return -1;
	}

	// Generate raw scanline stream and pack into stored DEFLATE blocks.
	uint32_t remaining = raw_size;
	PngRgbGen gen = {
		.img = img,
		.top_row = top_row,
		.bottom_row = bottom_row,
		.scanline = scanline,
		.row_bytes = row_bytes,
		.scanline_bytes = scanline_bytes,
		.y = 0,
		.have_cached_bottom = 0,
	};
	uint32_t scanline_pos = 0;

	while (remaining > 0) {
		const uint32_t len = (remaining > 65535u) ? 65535u : remaining;
		const uint8_t bfinal = (remaining <= 65535u) ? 1u : 0u;
		z[zp++] = bfinal; // BFINAL + BTYPE=00
		z[zp++] = (uint8_t)(len & 0xFFu);
		z[zp++] = (uint8_t)((len >> 8) & 0xFFu);
		const uint16_t nlen = (uint16_t)~(uint16_t)len;
		z[zp++] = (uint8_t)(nlen & 0xFFu);
		z[zp++] = (uint8_t)((nlen >> 8) & 0xFFu);

		uint32_t produced = 0;
		while (produced < len) {
			if (gen.y >= img->height) {
				// Should not happen if raw_size is correct.
				free(top_row);
				free(bottom_row);
				free(scanline);
				free(z);
				errno = EINVAL;
				return -1;
			}

			if (scanline_pos == 0) {
				if (png_fill_scanline(&gen) != 0) {
					free(top_row);
					free(bottom_row);
					free(scanline);
					free(z);
					errno = EINVAL;
					return -1;
				}
			}
			const uint32_t avail = scanline_bytes - scanline_pos;
			const uint32_t need = len - produced;
			const uint32_t take = (avail < need) ? avail : need;
			memcpy(z + zp, scanline + scanline_pos, take);
			adler32_update(&ad_a, &ad_b, scanline + scanline_pos, take);
			zp += take;
			produced += take;
			scanline_pos += take;
			if (scanline_pos == scanline_bytes) {
				scanline_pos = 0;
				gen.y += 1;
			}
		}

		remaining -= len;
	}

	free(top_row);
	free(bottom_row);
	free(scanline);

	// Adler-32 (big-endian)
	const uint32_t adler = (ad_b << 16) | ad_a;
	const uint32_t adler_be = be32(adler);
	memcpy(z + zp, &adler_be, 4);
	zp += 4;

	// IDAT (single chunk for simplicity).
	if (zp > 0xFFFFFFFFu) {
		free(z);
		errno = EFBIG;
		return -1;
	}
	if (write_chunk(fd, "IDAT", z, (uint32_t)zp) != 0) {
		free(z);
		return -1;
	}
	free(z);

	// IEND
	if (write_chunk(fd, "IEND", NULL, 0) != 0) return -1;
	return 0;
}
