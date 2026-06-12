#include "yuv2rgb_png.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>

#include "../common/os.h"
#include "../vp8/vp8_yuv_rgb.h"

#define PNG_SET_ERRNO(e) (errno = (e))

static void upsample_rgb_line_pair(const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u,
                                   const uint8_t* top_v, const uint8_t* cur_u, const uint8_t* cur_v,
                                   uint8_t* top_dst, uint8_t* bottom_dst, uint32_t len) {
	vp8_upsample_rgb_line_pair(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_dst, bottom_dst, len);
}

static inline uint32_t be32(uint32_t x) {
	return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) | ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}

static const uint32_t crc32_table[256] = {
	0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
	0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
	0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
	0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
	0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu,
	0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
	0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
	0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
	0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
	0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
	0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u,
	0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
	0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u,
	0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
	0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
	0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
	0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au,
	0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
	0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u,
	0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
	0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
	0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
	0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu,
	0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
	0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u,
	0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
	0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
	0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
	0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u,
	0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
	0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u,
	0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
	0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
	0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
	0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
	0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
	0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu,
	0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
	0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
	0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
	0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u,
	0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
	0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u,
	0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
	0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
	0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
	0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u,
	0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
	0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
	0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
	0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
	0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
	0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu,
	0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
	0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu,
	0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
	0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
	0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
	0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u,
	0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
	0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u,
	0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
	0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
	0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

// IDAT CRC covers every stored byte, so use slicing-by-16 for the large scanline payloads.
static uint32_t crc32_slicing_table[16][256];
static int crc32_slicing_ready;

static void crc32_slicing_init(void) {
	if (crc32_slicing_ready) return;
	for (uint32_t i = 0; i < 256u; i++) {
		crc32_slicing_table[0][i] = crc32_table[i];
	}
	for (uint32_t slice = 1; slice < 16u; slice++) {
		for (uint32_t i = 0; i < 256u; i++) {
			const uint32_t prev = crc32_slicing_table[slice - 1u][i];
			crc32_slicing_table[slice][i] = crc32_table[prev & 0xFFu] ^ (prev >> 8);
		}
	}
	crc32_slicing_ready = 1;
}

static inline uint32_t load32le(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t crc32_update_16(uint32_t crc, const uint8_t* buf) {
	crc ^= load32le(buf);
	const uint32_t b4 = load32le(buf + 4);
	const uint32_t b8 = load32le(buf + 8);
	const uint32_t b12 = load32le(buf + 12);
	return crc32_slicing_table[15][crc & 0xFFu] ^
	       crc32_slicing_table[14][(crc >> 8) & 0xFFu] ^
	       crc32_slicing_table[13][(crc >> 16) & 0xFFu] ^
	       crc32_slicing_table[12][crc >> 24] ^
	       crc32_slicing_table[11][b4 & 0xFFu] ^
	       crc32_slicing_table[10][(b4 >> 8) & 0xFFu] ^
	       crc32_slicing_table[9][(b4 >> 16) & 0xFFu] ^
	       crc32_slicing_table[8][b4 >> 24] ^
	       crc32_slicing_table[7][b8 & 0xFFu] ^
	       crc32_slicing_table[6][(b8 >> 8) & 0xFFu] ^
	       crc32_slicing_table[5][(b8 >> 16) & 0xFFu] ^
	       crc32_slicing_table[4][b8 >> 24] ^
	       crc32_slicing_table[3][b12 & 0xFFu] ^
	       crc32_slicing_table[2][(b12 >> 8) & 0xFFu] ^
	       crc32_slicing_table[1][(b12 >> 16) & 0xFFu] ^
	       crc32_slicing_table[0][b12 >> 24];
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
	crc ^= 0xFFFFFFFFu;
	if (len >= 16u) {
		crc32_slicing_init();
		while (len >= 16u) {
			crc = crc32_update_16(crc, buf);
			buf += 16;
			len -= 16u;
		}
	}
	for (size_t i = 0; i < len; i++) {
		crc = crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
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

static void crc32_adler32_update(uint32_t* crc_io, uint32_t* a, uint32_t* b, const uint8_t* buf, size_t len) {
	const uint32_t MOD = 65521u;
	const size_t NMAX = 5552u;
	uint32_t crc = *crc_io ^ 0xFFFFFFFFu;
	uint32_t aa = *a;
	uint32_t bb = *b;
	if (len >= 16u) crc32_slicing_init();
	while (len > 0) {
		size_t n = (len < NMAX) ? len : NMAX;
		len -= n;
		while (n >= 16u) {
			crc = crc32_update_16(crc, buf);
			aa += buf[0];
			bb += aa;
			aa += buf[1];
			bb += aa;
			aa += buf[2];
			bb += aa;
			aa += buf[3];
			bb += aa;
			aa += buf[4];
			bb += aa;
			aa += buf[5];
			bb += aa;
			aa += buf[6];
			bb += aa;
			aa += buf[7];
			bb += aa;
			aa += buf[8];
			bb += aa;
			aa += buf[9];
			bb += aa;
			aa += buf[10];
			bb += aa;
			aa += buf[11];
			bb += aa;
			aa += buf[12];
			bb += aa;
			aa += buf[13];
			bb += aa;
			aa += buf[14];
			bb += aa;
			aa += buf[15];
			bb += aa;
			buf += 16;
			n -= 16u;
		}
		while (n-- > 0) {
			crc = crc32_table[(crc ^ *buf) & 0xFFu] ^ (crc >> 8);
			aa += *buf++;
			bb += aa;
		}
		aa %= MOD;
		bb %= MOD;
	}
	*crc_io = crc ^ 0xFFFFFFFFu;
	*a = aa;
	*b = bb;
}

#define PNG_IDAT_BUF_SIZE 262144u

typedef struct {
	int fd;
	uint32_t crc;
	uint8_t* buf;
	size_t used;
	uint64_t raw_left;
	uint32_t block_left;
} PngIdatWriter;

static int png_idat_flush(PngIdatWriter* w) {
	if (w->used == 0) return 0;
	if (os_write_all(w->fd, w->buf, w->used) != 0) return -1;
	w->used = 0;
	return 0;
}

static int png_idat_write(PngIdatWriter* w, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	if (len == 0) return 0;
	w->crc = crc32_update(w->crc, p, len);
	while (len > 0) {
		if (w->used == 0 && len >= PNG_IDAT_BUF_SIZE) {
			if (os_write_all(w->fd, p, PNG_IDAT_BUF_SIZE) != 0) return -1;
			p += PNG_IDAT_BUF_SIZE;
			len -= PNG_IDAT_BUF_SIZE;
			continue;
		}
		const size_t space = PNG_IDAT_BUF_SIZE - w->used;
		const size_t n = (len < space) ? len : space;
		memcpy(w->buf + w->used, p, n);
		w->used += n;
		p += n;
		len -= n;
		if (w->used == PNG_IDAT_BUF_SIZE && png_idat_flush(w) != 0) return -1;
	}
	return 0;
}

static int png_idat_write_raw_payload(PngIdatWriter* w,
                                      uint32_t* ad_a,
                                      uint32_t* ad_b,
                                      const uint8_t* data,
                                      size_t len) {
	const uint8_t* p = data;
	if (len == 0) return 0;
	crc32_adler32_update(&w->crc, ad_a, ad_b, p, len);
	while (len > 0) {
		if (w->used == 0 && len >= PNG_IDAT_BUF_SIZE) {
			if (os_write_all(w->fd, p, PNG_IDAT_BUF_SIZE) != 0) return -1;
			p += PNG_IDAT_BUF_SIZE;
			len -= PNG_IDAT_BUF_SIZE;
			continue;
		}
		const size_t space = PNG_IDAT_BUF_SIZE - w->used;
		const size_t n = (len < space) ? len : space;
		memcpy(w->buf + w->used, p, n);
		w->used += n;
		p += n;
		len -= n;
		if (w->used == PNG_IDAT_BUF_SIZE && png_idat_flush(w) != 0) return -1;
	}
	return 0;
}

static int png_write_stored_header(PngIdatWriter* w, uint32_t len, uint8_t bfinal) {
	uint8_t hdr[5];
	hdr[0] = bfinal;
	hdr[1] = (uint8_t)(len & 0xFFu);
	hdr[2] = (uint8_t)((len >> 8) & 0xFFu);
	const uint16_t nlen = (uint16_t)~(uint16_t)len;
	hdr[3] = (uint8_t)(nlen & 0xFFu);
	hdr[4] = (uint8_t)((nlen >> 8) & 0xFFu);
	return png_idat_write(w, hdr, sizeof(hdr));
}

static int png_start_stored_block(PngIdatWriter* w) {
	const uint32_t block_len = (w->raw_left > 65535u) ? 65535u : (uint32_t)w->raw_left;
	const uint8_t bfinal = (w->raw_left == (uint64_t)block_len) ? 1u : 0u;
	if (png_write_stored_header(w, block_len, bfinal) != 0) return -1;
	w->block_left = block_len;
	return 0;
}

// Stored DEFLATE blocks may span PNG scanlines; packing the raw scanline stream into
// 64 KiB blocks avoids per-row block headers while preserving filter-0 RGB bytes.
static int png_write_raw_bytes(PngIdatWriter* w,
                               uint32_t* ad_a,
                               uint32_t* ad_b,
                               const uint8_t* data,
                               size_t len) {
	if ((uint64_t)len > w->raw_left) {
		PNG_SET_ERRNO(EINVAL);
		return -1;
	}
	while (len > 0) {
		if (w->block_left == 0) {
			if (png_start_stored_block(w) != 0) return -1;
		}
		const size_t n = (len < (size_t)w->block_left) ? len : (size_t)w->block_left;
		if (png_idat_write_raw_payload(w, ad_a, ad_b, data, n) != 0) return -1;
		data += n;
		len -= n;
		w->block_left -= (uint32_t)n;
		w->raw_left -= (uint64_t)n;
	}
	return 0;
}

static int png_reserve_contiguous_raw(PngIdatWriter* w, size_t len, uint8_t** out) {
	*out = NULL;
	if ((uint64_t)len > w->raw_left) {
		PNG_SET_ERRNO(EINVAL);
		return -1;
	}
	if (len == 0) return 0;
	if (w->block_left == 0 && png_start_stored_block(w) != 0) return -1;
	if (len > (size_t)w->block_left || len > PNG_IDAT_BUF_SIZE) return 0;
	if (PNG_IDAT_BUF_SIZE - w->used < len && png_idat_flush(w) != 0) return -1;
	*out = w->buf + w->used;
	return 0;
}

static int png_commit_contiguous_raw(PngIdatWriter* w, uint32_t* ad_a, uint32_t* ad_b, size_t len) {
	crc32_adler32_update(&w->crc, ad_a, ad_b, w->buf + w->used, len);
	w->used += len;
	w->block_left -= (uint32_t)len;
	w->raw_left -= (uint64_t)len;
	if (w->used == PNG_IDAT_BUF_SIZE && png_idat_flush(w) != 0) return -1;
	return 0;
}

int yuv420_write_png_fd(int fd, const Yuv420Image* img) {
	if (fd < 0 || !img || !img->y || !img->u || !img->v) {
		PNG_SET_ERRNO(EINVAL);
		return -1;
	}
	if (img->width == 0 || img->height == 0) {
		PNG_SET_ERRNO(EINVAL);
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

	if (img->width > (UINT32_MAX - 1u) / 3u) {
		PNG_SET_ERRNO(EFBIG);
		return -1;
	}
	const uint32_t row_bytes = img->width * 3u;
	const uint32_t scanline_bytes = 1u + row_bytes; // filter byte + RGB
	const uint64_t raw_size64 = (uint64_t)img->height * (uint64_t)scanline_bytes;
	if (raw_size64 > 0x7FFFFFFFu) {
		PNG_SET_ERRNO(EFBIG);
		return -1;
	}
	const uint64_t blocks = (raw_size64 + 65534u) / 65535u;
	const uint64_t zsize64 = 2u + raw_size64 + blocks * 5u + 4u;
	if (zsize64 > 0xFFFFFFFFu) {
		PNG_SET_ERRNO(EFBIG);
		return -1;
	}
	uint8_t* rows = (uint8_t*)malloc((size_t)scanline_bytes * 2u);
	uint8_t* idat_buf = (uint8_t*)malloc(PNG_IDAT_BUF_SIZE);
	if (!rows || !idat_buf) {
		free(rows);
		free(idat_buf);
		PNG_SET_ERRNO(ENOMEM);
		return -1;
	}
	uint8_t* top_scanline = rows;
	uint8_t* bottom_scanline = rows + scanline_bytes;
	uint8_t* top_row = top_scanline + 1u;
	uint8_t* bottom_row = bottom_scanline + 1u;
	top_scanline[0] = 0;
	bottom_scanline[0] = 0;

	uint8_t idat_hdr[8];
	uint32_t idat_len_be = be32((uint32_t)zsize64);
	memcpy(idat_hdr + 0, &idat_len_be, 4);
	memcpy(idat_hdr + 4, "IDAT", 4);
	if (os_write_all(fd, idat_hdr, sizeof(idat_hdr)) != 0) {
		free(rows);
		free(idat_buf);
		return -1;
	}

	PngIdatWriter idat = {
		.fd = fd,
		.crc = crc32_update(0, (const uint8_t*)"IDAT", 4),
		.buf = idat_buf,
		.used = 0,
		.raw_left = raw_size64,
		.block_left = 0,
	};

	uint32_t ad_a = 1u;
	uint32_t ad_b = 0u;

	// zlib header: 0x78 0x01 (no compression / fastest).
	static const uint8_t zlib_header[2] = {0x78u, 0x01u};
	if (png_idat_write(&idat, zlib_header, sizeof(zlib_header)) != 0) goto idat_error;

	{
		const uint8_t* y0 = img->y;
		const uint8_t* u0 = img->u;
		const uint8_t* v0 = img->v;
		uint8_t* scanline = NULL;
		if (png_reserve_contiguous_raw(&idat, scanline_bytes, &scanline) != 0) goto idat_error;
		if (scanline != NULL) {
			scanline[0] = 0;
			upsample_rgb_line_pair(y0, NULL, u0, v0, u0, v0, scanline + 1u, NULL, img->width);
			if (png_commit_contiguous_raw(&idat, &ad_a, &ad_b, scanline_bytes) != 0) goto idat_error;
		} else {
			upsample_rgb_line_pair(y0, NULL, u0, v0, u0, v0, top_row, NULL, img->width);
			if (png_write_raw_bytes(&idat, &ad_a, &ad_b, top_scanline, scanline_bytes) != 0) goto idat_error;
		}
	}

	const uint32_t ch = (img->height + 1u) >> 1;
	for (uint32_t y = 1; y < img->height; y += 2u) {
		const uint8_t* top_y = img->y + (size_t)y * img->stride_y;
		const uint8_t* bottom_y = (y + 1u < img->height) ? (img->y + (size_t)(y + 1u) * img->stride_y) : NULL;

		const uint32_t top_cy = y >> 1;
		const uint32_t cur_cy = (top_cy + 1u < ch) ? (top_cy + 1u) : (ch - 1u);
		const uint8_t* top_u = img->u + (size_t)top_cy * img->stride_uv;
		const uint8_t* top_v = img->v + (size_t)top_cy * img->stride_uv;
		const uint8_t* cur_u = img->u + (size_t)cur_cy * img->stride_uv;
		const uint8_t* cur_v = img->v + (size_t)cur_cy * img->stride_uv;

		const size_t out_bytes = bottom_y != NULL ? (size_t)scanline_bytes * 2u : (size_t)scanline_bytes;
		uint8_t* scanlines = NULL;
		if (png_reserve_contiguous_raw(&idat, out_bytes, &scanlines) != 0) goto idat_error;
		if (scanlines != NULL) {
			scanlines[0] = 0;
			if (bottom_y != NULL) scanlines[scanline_bytes] = 0;
			upsample_rgb_line_pair(top_y, bottom_y, top_u, top_v, cur_u, cur_v,
			                       scanlines + 1u,
			                       bottom_y != NULL ? scanlines + scanline_bytes + 1u : NULL,
			                       img->width);
			if (png_commit_contiguous_raw(&idat, &ad_a, &ad_b, out_bytes) != 0) goto idat_error;
		} else {
			upsample_rgb_line_pair(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_row, bottom_row, img->width);
			if (png_write_raw_bytes(&idat, &ad_a, &ad_b, top_scanline, out_bytes) != 0) goto idat_error;
		}
	}
	if (idat.raw_left != 0 || idat.block_left != 0) {
		PNG_SET_ERRNO(EINVAL);
		goto idat_error;
	}

	const uint32_t adler = (ad_b << 16) | ad_a;
	const uint32_t adler_be = be32(adler);
	if (png_idat_write(&idat, &adler_be, 4) != 0) goto idat_error;
	if (png_idat_flush(&idat) != 0) goto idat_error;

	const uint32_t idat_crc_be = be32(idat.crc);
	if (os_write_all(fd, &idat_crc_be, 4) != 0) goto idat_error;
	free(rows);
	free(idat_buf);

	// IEND
	if (write_chunk(fd, "IEND", NULL, 0) != 0) return -1;
	return 0;

idat_error:
	free(rows);
	free(idat_buf);
	return -1;
}
