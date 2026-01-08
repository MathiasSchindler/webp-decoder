#include "enc_png.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- tiny helpers ---

static uint32_t be32(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
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

// --- adler32 (zlib) ---

static uint32_t adler32(const uint8_t* data, size_t n) {
	uint32_t a = 1;
	uint32_t b = 0;
	for (size_t i = 0; i < n; i++) {
		a = (a + data[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return (b << 16) | a;
}

// --- DEFLATE inflate (zlib wrapper) ---

typedef struct BitReader {
	const uint8_t* p;
	const uint8_t* end;
	uint64_t bitbuf;
	int bitcount;
} BitReader;

static int br_fill(BitReader* br, int need) {
	while (br->bitcount < need) {
		if (br->p >= br->end) return -1;
		br->bitbuf |= (uint64_t)(*br->p++) << br->bitcount;
		br->bitcount += 8;
	}
	return 0;
}

static int br_read_bits(BitReader* br, int n, uint32_t* out) {
	if (n == 0) {
		*out = 0;
		return 0;
	}
	if (br_fill(br, n) != 0) return -1;
	*out = (uint32_t)(br->bitbuf & ((1ull << n) - 1ull));
	br->bitbuf >>= n;
	br->bitcount -= n;
	return 0;
}

static int br_align_byte(BitReader* br) {
	int drop = br->bitcount & 7;
	if (drop) {
		br->bitbuf >>= drop;
		br->bitcount -= drop;
	}
	return 0;
}

typedef struct Huff {
	// Canonical Huffman decode: small linear decode is OK for now.
	// code -> symbol via (code,len) matching.
	uint16_t sym[288];
	uint8_t len[288];
	int count;
} Huff;

static int huff_build(Huff* h, const uint8_t* lengths, int count) {
	// Build canonical codes; store per-symbol lengths; decoding does bit-by-bit match.
	h->count = count;
	for (int i = 0; i < count; i++) {
		h->sym[i] = (uint16_t)i;
		h->len[i] = lengths[i];
	}
	return 0;
}

static int huff_decode(BitReader* br, const Huff* h, int* out_sym) {
	// Slow but simple: read up to 15 bits, try all symbols with matching length.
	// For Milestone 0 correctness > speed.
	uint32_t code = 0;
	for (int n = 1; n <= 15; n++) {
		uint32_t bit;
		if (br_read_bits(br, 1, &bit) != 0) return -1;
		code |= bit << (n - 1);

		// Canonical Huffman is defined MSB-first, but DEFLATE transmits LSB-first.
		// We are accumulating LSB-first codes; to match canonical ordering we'd need a table.
		// Instead, we build decoding using DEFLATE's bit-reversed codes on the fly.
		// We'll compute the bit-reversed value for n bits and match it against canonical codes.
		uint32_t rev = 0;
		for (int i = 0; i < n; i++) rev = (rev << 1) | ((code >> i) & 1u);

		// Build canonical code ranges each call is expensive; keep it simple but correct:
		// Generate canonical codes from lengths and compare.
		uint16_t bl_count[16] = {0};
		for (int i = 0; i < h->count; i++) {
			uint8_t l = h->len[i];
			if (l <= 15) bl_count[l]++;
		}
		uint16_t next_code[16] = {0};
		uint16_t c = 0;
		for (int bits = 1; bits <= 15; bits++) {
			c = (uint16_t)((c + bl_count[bits - 1]) << 1);
			next_code[bits] = c;
		}
		for (int sym = 0; sym < h->count; sym++) {
			uint8_t l = h->len[sym];
			if (l != (uint8_t)n) continue;
			uint16_t canon = next_code[n]++;
			if (canon == (uint16_t)rev) {
				*out_sym = sym;
				return 0;
			}
		}
	}
	return -1;
}

static void build_fixed_huffman(Huff* litlen, Huff* dist) {
	uint8_t ll[288];
	for (int i = 0; i <= 143; i++) ll[i] = 8;
	for (int i = 144; i <= 255; i++) ll[i] = 9;
	for (int i = 256; i <= 279; i++) ll[i] = 7;
	for (int i = 280; i <= 287; i++) ll[i] = 8;
	(void)huff_build(litlen, ll, 288);

	uint8_t dl[32];
	for (int i = 0; i < 32; i++) dl[i] = 5;
	(void)huff_build(dist, dl, 32);
}

static const int LEN_BASE[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
	163, 195, 227, 258,
};
static const int LEN_EXTRA[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const int DIST_BASE[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
	3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const int DIST_EXTRA[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
	13, 13,
};

static int inflate_zlib(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
	if (in_len < 6) return -1;
	uint8_t cmf = in[0];
	uint8_t flg = in[1];
	if ((cmf & 0x0F) != 8) return -1;          // deflate
	if (((cmf << 8) | flg) % 31 != 0) return -1;
	if (flg & 0x20) return -1; // no preset dictionary

	BitReader br = {.p = in + 2, .end = in + in_len - 4, .bitbuf = 0, .bitcount = 0};
	size_t out_pos = 0;

	int final_block = 0;
	while (!final_block) {
		uint32_t bfinal, btype;
		if (br_read_bits(&br, 1, &bfinal) != 0) return -1;
		if (br_read_bits(&br, 2, &btype) != 0) return -1;
		final_block = (int)bfinal;

		if (btype == 0) {
			// stored
			br_align_byte(&br);
			if ((size_t)(br.end - br.p) < 4) return -1;
			uint16_t len = (uint16_t)(br.p[0] | (br.p[1] << 8));
			uint16_t nlen = (uint16_t)(br.p[2] | (br.p[3] << 8));
			br.p += 4;
			if ((uint16_t)(len ^ 0xFFFFu) != nlen) return -1;
			if ((size_t)(br.end - br.p) < len) return -1;
			if (out_pos + len > out_len) return -1;
			memcpy(out + out_pos, br.p, len);
			br.p += len;
			out_pos += len;
			continue;
		}

		Huff litlen, dist;
		uint8_t ll_len[288] = {0};
		uint8_t d_len[32] = {0};

		if (btype == 1) {
			build_fixed_huffman(&litlen, &dist);
		} else if (btype == 2) {
			uint32_t HLIT, HDIST, HCLEN;
			if (br_read_bits(&br, 5, &HLIT) != 0) return -1;
			if (br_read_bits(&br, 5, &HDIST) != 0) return -1;
			if (br_read_bits(&br, 4, &HCLEN) != 0) return -1;
			HLIT += 257;
			HDIST += 1;
			HCLEN += 4;

			static const uint8_t CL_ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
			uint8_t cl_len[19] = {0};
			for (uint32_t i = 0; i < HCLEN; i++) {
				uint32_t v;
				if (br_read_bits(&br, 3, &v) != 0) return -1;
				cl_len[CL_ORDER[i]] = (uint8_t)v;
			}
			Huff cl_h;
			(void)huff_build(&cl_h, cl_len, 19);

			uint32_t idx = 0;
			uint8_t prev = 0;
			while (idx < HLIT + HDIST) {
				int sym;
				if (huff_decode(&br, &cl_h, &sym) != 0) return -1;
				if (sym <= 15) {
					prev = (uint8_t)sym;
					if (idx < HLIT) ll_len[idx] = prev;
					else d_len[idx - HLIT] = prev;
					idx++;
				} else if (sym == 16) {
					uint32_t rep;
					if (br_read_bits(&br, 2, &rep) != 0) return -1;
					rep += 3;
					for (uint32_t j = 0; j < rep; j++) {
						if (idx >= HLIT + HDIST) return -1;
						if (idx < HLIT) ll_len[idx] = prev;
						else d_len[idx - HLIT] = prev;
						idx++;
					}
				} else if (sym == 17) {
					uint32_t rep;
					if (br_read_bits(&br, 3, &rep) != 0) return -1;
					rep += 3;
					prev = 0;
					for (uint32_t j = 0; j < rep; j++) {
						if (idx >= HLIT + HDIST) return -1;
						if (idx < HLIT) ll_len[idx] = 0;
						else d_len[idx - HLIT] = 0;
						idx++;
					}
				} else if (sym == 18) {
					uint32_t rep;
					if (br_read_bits(&br, 7, &rep) != 0) return -1;
					rep += 11;
					prev = 0;
					for (uint32_t j = 0; j < rep; j++) {
						if (idx >= HLIT + HDIST) return -1;
						if (idx < HLIT) ll_len[idx] = 0;
						else d_len[idx - HLIT] = 0;
						idx++;
					}
				} else {
					return -1;
				}
			}

			(void)huff_build(&litlen, ll_len, (int)HLIT);
			(void)huff_build(&dist, d_len, (int)HDIST);
		} else {
			return -1;
		}

		for (;;) {
			int sym;
			if (huff_decode(&br, &litlen, &sym) != 0) return -1;
			if (sym < 256) {
				if (out_pos >= out_len) return -1;
				out[out_pos++] = (uint8_t)sym;
				continue;
			}
			if (sym == 256) break;
			int len_sym = sym - 257;
			if (len_sym < 0 || len_sym >= 29) return -1;
			uint32_t extra;
			if (br_read_bits(&br, LEN_EXTRA[len_sym], &extra) != 0) return -1;
			int length = LEN_BASE[len_sym] + (int)extra;

			int dist_sym;
			if (huff_decode(&br, &dist, &dist_sym) != 0) return -1;
			if (dist_sym < 0 || dist_sym >= 30) return -1;
			uint32_t de;
			if (br_read_bits(&br, DIST_EXTRA[dist_sym], &de) != 0) return -1;
			int distance = DIST_BASE[dist_sym] + (int)de;
			if (distance <= 0) return -1;
			if ((size_t)distance > out_pos) return -1;
			if (out_pos + (size_t)length > out_len) return -1;

			size_t from = out_pos - (size_t)distance;
			for (int i = 0; i < length; i++) {
				out[out_pos++] = out[from++];
			}
		}
	}

	uint32_t want = be32(in + in_len - 4);
	uint32_t got = adler32(out, out_pos);
	if (want != got) return -1;
	return (out_pos == out_len) ? 0 : -1;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
	int p = (int)a + (int)b - (int)c;
	int pa = abs(p - (int)a);
	int pb = abs(p - (int)b);
	int pc = abs(p - (int)c);
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static int unfilter(uint8_t* out, const uint8_t* in, uint32_t w, uint32_t h, int bpp) {
	size_t stride = (size_t)w * (size_t)bpp;
	const uint8_t* prev = NULL;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t ft = *in++;
		uint8_t* row = out + (size_t)y * stride;
		switch (ft) {
			case 0: // None
				memcpy(row, in, stride);
				break;
			case 1: // Sub
				for (size_t i = 0; i < stride; i++) {
					uint8_t left = (i >= (size_t)bpp) ? row[i - (size_t)bpp] : 0;
					row[i] = (uint8_t)(in[i] + left);
				}
				break;
			case 2: // Up
				for (size_t i = 0; i < stride; i++) {
					uint8_t up = prev ? prev[i] : 0;
					row[i] = (uint8_t)(in[i] + up);
				}
				break;
			case 3: // Average
				for (size_t i = 0; i < stride; i++) {
					uint8_t left = (i >= (size_t)bpp) ? row[i - (size_t)bpp] : 0;
					uint8_t up = prev ? prev[i] : 0;
					row[i] = (uint8_t)(in[i] + ((uint8_t)(((int)left + (int)up) / 2)));
				}
				break;
			case 4: // Paeth
				for (size_t i = 0; i < stride; i++) {
					uint8_t left = (i >= (size_t)bpp) ? row[i - (size_t)bpp] : 0;
					uint8_t up = prev ? prev[i] : 0;
					uint8_t up_left = (prev && i >= (size_t)bpp) ? prev[i - (size_t)bpp] : 0;
					row[i] = (uint8_t)(in[i] + paeth(left, up, up_left));
				}
				break;
			default:
				return -1;
		}
		in += stride;
		prev = row;
	}
	return 0;
}

int enc_png_read_file(const char* path, EncPngImage* out_img) {
	if (!out_img) return -1;
	memset(out_img, 0, sizeof(*out_img));

	uint8_t* file = NULL;
	size_t file_size = 0;
	if (read_entire_file(path, &file, &file_size) != 0) return -1;

	static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (file_size < 8 || memcmp(file, SIG, 8) != 0) {
		free(file);
		return -1;
	}

	size_t off = 8;
	uint32_t width = 0, height = 0;
	uint8_t bit_depth = 0, color_type = 0, comp = 0, filt = 0, interlace = 0;
	uint8_t* idat = NULL;
	size_t idat_size = 0;

	int saw_ihdr = 0;
	int saw_iend = 0;
	while (off + 12 <= file_size) {
		uint32_t len = be32(file + off);
		uint32_t typ = be32(file + off + 4);
		off += 8;
		if (off + len + 4 > file_size) {
			free(idat);
			free(file);
			return -1;
		}
		const uint8_t* data = file + off;
		off += len;
		// skip CRC
		off += 4;

		if (typ == 0x49484452u) { // IHDR
			if (len != 13 || saw_ihdr) {
				free(idat);
				free(file);
				return -1;
			}
			width = be32(data);
			height = be32(data + 4);
			bit_depth = data[8];
			color_type = data[9];
			comp = data[10];
			filt = data[11];
			interlace = data[12];
			saw_ihdr = 1;
			continue;
		}
		if (!saw_ihdr) {
			free(idat);
			free(file);
			return -1;
		}

		if (typ == 0x49444154u) { // IDAT
			uint8_t* grown = (uint8_t*)realloc(idat, idat_size + len);
			if (!grown) {
				free(idat);
				free(file);
				errno = ENOMEM;
				return -1;
			}
			idat = grown;
			memcpy(idat + idat_size, data, len);
			idat_size += len;
			continue;
		}
		if (typ == 0x49454E44u) { // IEND
			saw_iend = 1;
			break;
		}
		// ignore other chunks for now
	}

	free(file);

	if (!saw_ihdr || !saw_iend || idat_size == 0) {
		free(idat);
		return -1;
	}
	if (width == 0 || height == 0) {
		free(idat);
		return -1;
	}
	if (bit_depth != 8) {
		free(idat);
		return -1;
	}
	if (!(color_type == 2 || color_type == 6)) {
		free(idat);
		return -1;
	}
	if (comp != 0 || filt != 0 || interlace != 0) {
		free(idat);
		return -1;
	}

	int channels = (color_type == 2) ? 3 : 4;
	size_t stride = (size_t)width * (size_t)channels;
	size_t scan = 1 + stride;
	if (height > (SIZE_MAX / scan)) {
		free(idat);
		return -1;
	}
	size_t inflated_size = (size_t)height * scan;

	uint8_t* inflated = (uint8_t*)malloc(inflated_size);
	if (!inflated) {
		free(idat);
		errno = ENOMEM;
		return -1;
	}

	if (inflate_zlib(idat, idat_size, inflated, inflated_size) != 0) {
		free(inflated);
		free(idat);
		return -1;
	}
	free(idat);

	uint8_t* pix = (uint8_t*)malloc((size_t)height * stride);
	if (!pix) {
		free(inflated);
		errno = ENOMEM;
		return -1;
	}
	if (unfilter(pix, inflated, width, height, channels) != 0) {
		free(pix);
		free(inflated);
		return -1;
	}
	free(inflated);

	out_img->width = width;
	out_img->height = height;
	out_img->channels = (uint8_t)channels;
	out_img->data = pix;
	return 0;
}

void enc_png_free(EncPngImage* img) {
	if (!img) return;
	free(img->data);
	img->data = NULL;
	img->width = 0;
	img->height = 0;
	img->channels = 0;
}
