#include "common/fmt.h"
#include "common/os.h"
#include "m01_container/webp_container.h"
#include "m02_vp8_header/vp8_header.h"
#include "m04_frame_header_full/vp8_frame_header_basic.h"
#include "m05_tokens/vp8_tokens.h"
#include "m06_recon/vp8_recon.h"

#ifndef DECODER_TINY
#include "m08_yuv2rgb_ppm/yuv2rgb_ppm.h"
#include "m09_png/yuv2rgb_png.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
	fmt_write_str(2, "Usage:\n");
	fmt_write_str(2, "  decoder -info <file.webp>\n");
	fmt_write_str(2, "  decoder -yuv <file.webp> <out.i420>\n");
	fmt_write_str(2, "  decoder -yuvf <file.webp> <out.i420>\n");

#ifndef DECODER_TINY
	fmt_write_str(2, "  decoder -probe <file.webp>\n");
	fmt_write_str(2, "  decoder -dump_mb <file.webp> [mb_index]\n");
	fmt_write_str(2, "  decoder -ppm <file.webp> <out.ppm>\n");
	fmt_write_str(2, "  decoder -png <file.webp> <out.png>\n");
	fmt_write_str(2, "  decoder -diff_mb <file.webp> <oracle.i420>\n");
#endif
}

#ifndef DECODER_TINY
static void print_mb_mode_u8(uint8_t v, const char* const* names, uint32_t n) {
	if ((uint32_t)v < n && names[v]) {
		fmt_write_str(1, names[v]);
	} else {
		fmt_write_u32(1, (uint32_t)v);
	}
}

static void print_coeff_stats(const Vp8CoeffStats* cs) {
	fmt_write_str(1, "  Coeff hash:       ");
	fmt_write_u64(1, cs->coeff_hash_fnv1a64);
	fmt_write_nl(1);
	fmt_write_str(1, "  Part0 bytes used: ");
	fmt_write_u32(1, cs->part0_bytes_used);
	fmt_write_str(1, " /");
	fmt_write_u32(1, cs->part0_size_bytes);
	fmt_write_nl(1);
	fmt_write_str(1, "  Token bytes used: ");
	fmt_write_u32(1, cs->token_part_bytes_used);
	fmt_write_str(1, " /");
	fmt_write_u32(1, cs->token_part_size_bytes);
	fmt_write_nl(1);
	fmt_write_str(1, "  Part0 overread:   ");
	fmt_write_str(1, cs->part0_overread ? "Yes\n" : "No\n");
	fmt_write_str(1, "  Part0 overread b: ");
	fmt_write_u32(1, cs->part0_overread_bytes);
	fmt_write_nl(1);
	fmt_write_str(1, "  Token overread:   ");
	fmt_write_str(1, cs->token_overread ? "Yes\n" : "No\n");
	fmt_write_str(1, "  Token overread b: ");
	fmt_write_u32(1, cs->token_overread_bytes);
	fmt_write_nl(1);
	if (cs->token_overread && cs->token_overread_mb_index != 0xFFFFFFFFu) {
		fmt_write_str(1, "  Token overread @: MB ");
		fmt_write_u32(1, cs->token_overread_mb_index);
		fmt_write_str(1, " plane=");
		switch (cs->token_overread_plane) {
			case 0: fmt_write_str(1, "Y"); break;
			case 1: fmt_write_str(1, "Y2"); break;
			case 2: fmt_write_str(1, "U"); break;
			case 3: fmt_write_str(1, "V"); break;
			default: fmt_write_str(1, "?"); break;
		}
		fmt_write_str(1, " blk=");
		fmt_write_u32(1, cs->token_overread_block_index);
		fmt_write_str(1, " i=");
		fmt_write_u32(1, cs->token_overread_coeff_i);
		fmt_write_str(1, " stage=");
		fmt_write_u32(1, cs->token_overread_stage);
		fmt_write_nl(1);
	}
}

#endif

#ifndef DECODER_TINY
static int cmd_probe(const char* path) {
	ByteSpan file;
	if (os_map_file_readonly(path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8CoeffStats base;
	if (vp8_decode_coeff_stats(vp8_payload, &base) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	const size_t pad = 2048;
	uint8_t* buf = (uint8_t*)malloc(vp8_payload.size + pad);
	if (!buf) {
		fmt_write_str(2, "error: out of memory\n");
		os_unmap_file(file);
		return 1;
	}
	memcpy(buf, vp8_payload.data, vp8_payload.size);

	Vp8CoeffStats zpad;
	memset(buf + vp8_payload.size, 0x00, pad);
	ByteSpan vp8_zpad = {.data = buf, .size = vp8_payload.size + pad};
	if (vp8_decode_coeff_stats(vp8_zpad, &zpad) != 0) {
		fmt_write_str(2, "error: padded(0x00) decode failed\n");
		free(buf);
		os_unmap_file(file);
		return 1;
	}

	Vp8CoeffStats fpad;
	memset(buf + vp8_payload.size, 0xFF, pad);
	ByteSpan vp8_fpad = {.data = buf, .size = vp8_payload.size + pad};
	if (vp8_decode_coeff_stats(vp8_fpad, &fpad) != 0) {
		fmt_write_str(2, "error: padded(0xFF) decode failed\n");
		free(buf);
		os_unmap_file(file);
		return 1;
	}

	fmt_write_str(1, "File: ");
	fmt_write_str(1, path);
	fmt_write_nl(1);

	fmt_write_str(1, "Baseline:\n");
	print_coeff_stats(&base);

	fmt_write_str(1, "Padded (0x00, +2048 bytes):\n");
	print_coeff_stats(&zpad);

	fmt_write_str(1, "Padded (0xFF, +2048 bytes):\n");
	print_coeff_stats(&fpad);

	fmt_write_str(1, "Probe result (hash equality):\n");
	fmt_write_str(1, "  baseline vs 0x00: ");
	fmt_write_str(1, (base.coeff_hash_fnv1a64 == zpad.coeff_hash_fnv1a64) ? "SAME\n" : "DIFF\n");
	fmt_write_str(1, "  0x00 vs 0xFF:     ");
	fmt_write_str(1, (zpad.coeff_hash_fnv1a64 == fpad.coeff_hash_fnv1a64) ? "SAME\n" : "DIFF\n");

	free(buf);
	os_unmap_file(file);
	return 0;
}

static int cmd_dump_mb(const char* path, uint32_t mb_index) {
	ByteSpan file;
	if (os_map_file_readonly(path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8DecodedFrame f;
	if (vp8_decode_decoded_frame(vp8_payload, &f) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	if (mb_index >= f.mb_total) {
		fmt_write_str(2, "error: mb_index out of range\n");
		vp8_decoded_frame_free(&f);
		os_unmap_file(file);
		return 1;
	}

	static const char* const ymode_names[5] = {"DC", "V", "H", "TM", "B_PRED"};
	static const char* const uv_names[4] = {"DC", "V", "H", "TM"};
	static const char* const bmode_names[10] = {"B_DC", "B_TM", "B_VE", "B_HE", "B_LD", "B_RD", "B_VR", "B_VL", "B_HD",
	                                         "B_HU"};

	fmt_write_str(1, "File: ");
	fmt_write_str(1, path);
	fmt_write_nl(1);
	fmt_write_str(1, "MB index: ");
	fmt_write_u32(1, mb_index);
	fmt_write_str(1, " (cols=");
	fmt_write_u32(1, f.mb_cols);
	fmt_write_str(1, ", rows=");
	fmt_write_u32(1, f.mb_rows);
	fmt_write_str(1, ")\n");

	fmt_write_str(1, "  q_index:    ");
	fmt_write_u32(1, f.q_index);
	fmt_write_nl(1);
	fmt_write_str(1, "  dq (y1dc,y2dc,y2ac,uvdc,uvac): ");
	fmt_write_i32(1, f.y1_dc_delta_q);
	fmt_write_str(1, " ");
	fmt_write_i32(1, f.y2_dc_delta_q);
	fmt_write_str(1, " ");
	fmt_write_i32(1, f.y2_ac_delta_q);
	fmt_write_str(1, " ");
	fmt_write_i32(1, f.uv_dc_delta_q);
	fmt_write_str(1, " ");
	fmt_write_i32(1, f.uv_ac_delta_q);
	fmt_write_nl(1);

	fmt_write_str(1, "  segmentation_enabled: ");
	fmt_write_u32(1, f.segmentation_enabled);
	fmt_write_nl(1);
	if (f.segmentation_enabled) {
		fmt_write_str(1, "  segmentation_abs:     ");
		fmt_write_u32(1, f.segmentation_abs);
		fmt_write_nl(1);
		fmt_write_str(1, "  seg_quant_idx:        ");
		for (int i = 0; i < 4; i++) {
			fmt_write_i32(1, f.seg_quant_idx[i]);
			fmt_write_str(1, (i == 3) ? "\n" : " ");
		}
	}

	fmt_write_str(1, "  segment_id: ");
	fmt_write_u32(1, f.segment_id[mb_index]);
	fmt_write_nl(1);
	fmt_write_str(1, "  skip_coeff: ");
	fmt_write_u32(1, f.skip_coeff[mb_index]);
	fmt_write_nl(1);
	fmt_write_str(1, "  ymode:      ");
	print_mb_mode_u8(f.ymode[mb_index], ymode_names, 5);
	fmt_write_nl(1);
	fmt_write_str(1, "  uv_mode:    ");
	print_mb_mode_u8(f.uv_mode[mb_index], uv_names, 4);
	fmt_write_nl(1);

	if (f.ymode[mb_index] == 4) {
		fmt_write_str(1, "  bmode 4x4:\n");
		for (uint32_t rr = 0; rr < 4; rr++) {
			fmt_write_str(1, "    ");
			for (uint32_t cc = 0; cc < 4; cc++) {
				uint8_t m = f.bmode[(size_t)mb_index * 16u + (size_t)(rr * 4u + cc)];
				print_mb_mode_u8(m, bmode_names, 10);
				fmt_write_str(1, (cc == 3) ? "\n" : " ");
			}
		}
	}

	// Print coefficient samples (enough to spot obvious corruption).
	const int16_t* y2 = f.coeff_y2 + (size_t)mb_index * 16u;
	fmt_write_str(1, "  Y2 coeff[0..15]: ");
	for (int i = 0; i < 16; i++) {
		fmt_write_i32(1, y2[i]);
		fmt_write_str(1, (i == 15) ? "\n" : " ");
	}
	const int16_t* y0 = f.coeff_y + ((size_t)mb_index * 16u + 0u) * 16u;
	fmt_write_str(1, "  Y block0 coeff[0..15]: ");
	for (int i = 0; i < 16; i++) {
		fmt_write_i32(1, y0[i]);
		fmt_write_str(1, (i == 15) ? "\n" : " ");
	}
	const int16_t* u0 = f.coeff_u + ((size_t)mb_index * 4u + 0u) * 16u;
	fmt_write_str(1, "  U block0 coeff[0..15]: ");
	for (int i = 0; i < 16; i++) {
		fmt_write_i32(1, u0[i]);
		fmt_write_str(1, (i == 15) ? "\n" : " ");
	}
	const int16_t* v0 = f.coeff_v + ((size_t)mb_index * 4u + 0u) * 16u;
	fmt_write_str(1, "  V block0 coeff[0..15]: ");
	for (int i = 0; i < 16; i++) {
		fmt_write_i32(1, v0[i]);
		fmt_write_str(1, (i == 15) ? "\n" : " ");
	}

	// Quick view of chroma DCs across all 4 sub-blocks.
	fmt_write_str(1, "  U DCs: ");
	for (int b = 0; b < 4; b++) {
		const int16_t* ub = f.coeff_u + ((size_t)mb_index * 4u + (size_t)b) * 16u;
		fmt_write_i32(1, ub[0]);
		fmt_write_str(1, (b == 3) ? "\n" : " ");
	}
	fmt_write_str(1, "  V DCs: ");
	for (int b = 0; b < 4; b++) {
		const int16_t* vb = f.coeff_v + ((size_t)mb_index * 4u + (size_t)b) * 16u;
		fmt_write_i32(1, vb[0]);
		fmt_write_str(1, (b == 3) ? "\n" : " ");
	}

	// Also show global stats summary relevant to the overread concerns.
	fmt_write_str(1, "  Token overread: ");
	fmt_write_str(1, f.stats.token_overread ? "Yes\n" : "No\n");
	fmt_write_str(1, "  Token overread b: ");
	fmt_write_u32(1, f.stats.token_overread_bytes);
	fmt_write_nl(1);

	vp8_decoded_frame_free(&f);
	os_unmap_file(file);
	return 0;
}
#endif

static int cmd_info(const char* path) {
	ByteSpan file;
	if (os_map_file_readonly(path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	fmt_write_str(1, "File: ");
	fmt_write_str(1, path);
	fmt_write_nl(1);

	fmt_write_str(1, "RIFF size: ");
	fmt_write_u32(1, c.riff_size);
	fmt_write_str(1, " (expected total ");
	fmt_write_size(1, (size_t)c.riff_size + 8);
	fmt_write_str(1, ", actual ");
	fmt_write_size(1, c.actual_size);
	fmt_write_str(1, ")\n");

	size_t vp8_chunk_header_off = (c.vp8_chunk_offset >= 8) ? (c.vp8_chunk_offset - 8) : 0;
	uint32_t vp8_total_len = c.vp8_chunk_size + 8;

	fmt_write_str(1, "Chunk VP8  at offset ");
	fmt_write_size(1, vp8_chunk_header_off);
	fmt_write_str(1, ", length ");
	fmt_write_u32(1, vp8_total_len);
	fmt_write_nl(1);

	fmt_write_str(1, "  (payload offset ");
	fmt_write_size(1, c.vp8_chunk_offset);
	fmt_write_str(1, ", payload length ");
	fmt_write_u32(1, c.vp8_chunk_size);
	fmt_write_str(1, ")\n");

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};
	Vp8KeyFrameHeader h;
	if (vp8_parse_keyframe_header(vp8_payload, &h) != 0) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}
	{
		fmt_write_str(1, "  Parsing lossy bitstream...\n");
		fmt_write_str(1, "  Key frame:        ");
		fmt_write_str(1, h.is_key_frame ? "Yes\n" : "No\n");
		fmt_write_str(1, "  Profile:          ");
		fmt_write_u32(1, h.profile);
		fmt_write_nl(1);
		fmt_write_str(1, "  Display:          ");
		fmt_write_str(1, h.show_frame ? "Yes\n" : "No\n");
		fmt_write_str(1, "  Part. 0 length:   ");
		fmt_write_u32(1, (uint32_t)h.first_partition_len);
		fmt_write_nl(1);
		fmt_write_str(1, "  Width:            ");
		fmt_write_u32(1, h.width);
		fmt_write_nl(1);
		fmt_write_str(1, "  X scale:          ");
		fmt_write_u32(1, h.x_scale);
		fmt_write_nl(1);
		fmt_write_str(1, "  Height:           ");
		fmt_write_u32(1, h.height);
		fmt_write_nl(1);
		fmt_write_str(1, "  Y scale:          ");
		fmt_write_u32(1, h.y_scale);
		fmt_write_nl(1);

		Vp8FrameHeaderBasic fh;
		if (vp8_parse_frame_header_basic(vp8_payload, &fh) != 0) {
			fmt_write_str(2, "error: VP8 frame header parse failed\n");
			os_unmap_file(file);
			return 1;
		}
		{
			fmt_write_str(1, "  Color space:      ");
			fmt_write_u32(1, fh.color_space);
			fmt_write_nl(1);
			fmt_write_str(1, "  Clamp type:       ");
			fmt_write_u32(1, fh.clamp_type);
			fmt_write_nl(1);
			fmt_write_str(1, "  Use segment:      ");
			fmt_write_u32(1, fh.use_segment);
			fmt_write_nl(1);
			fmt_write_str(1, "  Simple filter:    ");
			fmt_write_u32(1, fh.simple_filter);
			fmt_write_nl(1);
			fmt_write_str(1, "  Level:            ");
			fmt_write_u32(1, fh.filter_level);
			fmt_write_nl(1);
			fmt_write_str(1, "  Sharpness:        ");
			fmt_write_u32(1, fh.sharpness);
			fmt_write_nl(1);
			fmt_write_str(1, "  Use lf delta:     ");
			fmt_write_u32(1, fh.use_lf_delta);
			fmt_write_nl(1);
			fmt_write_str(1, "  Total partitions: ");
			fmt_write_u32(1, fh.total_partitions);
			fmt_write_nl(1);
			// If multiple token partitions exist, print their lengths.
			if (fh.total_partitions > 1) {
				for (uint32_t i = 1; i < fh.total_partitions; i++) {
					fmt_write_str(1, "  Part. ");
					fmt_write_u32(1, i);
					fmt_write_str(1, " length:   ");
					fmt_write_u32(1, fh.part_sizes[i]);
					fmt_write_nl(1);
				}
			}
			fmt_write_str(1, "  Base Q:           ");
			fmt_write_u32(1, fh.base_q);
			fmt_write_nl(1);
			fmt_write_str(1, "  DQ Y1 DC:         ");
			fmt_write_i32(1, fh.dq_y1_dc);
			fmt_write_nl(1);
			fmt_write_str(1, "  DQ Y2 DC:         ");
			fmt_write_i32(1, fh.dq_y2_dc);
			fmt_write_nl(1);
			fmt_write_str(1, "  DQ Y2 AC:         ");
			fmt_write_i32(1, fh.dq_y2_ac);
			fmt_write_nl(1);
			fmt_write_str(1, "  DQ UV DC:         ");
			fmt_write_i32(1, fh.dq_uv_dc);
			fmt_write_nl(1);
			fmt_write_str(1, "  DQ UV AC:         ");
			fmt_write_i32(1, fh.dq_uv_ac);
			fmt_write_nl(1);
		}


#ifndef DECODER_TINY
		Vp8CoeffStats cs;
		if (vp8_decode_coeff_stats(vp8_payload, &cs) != 0) {
			fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
			os_unmap_file(file);
			return 1;
		}
		{
			fmt_write_str(1, "  MB cols:          ");
			fmt_write_u32(1, cs.mb_cols);
			fmt_write_nl(1);
			fmt_write_str(1, "  MB rows:          ");
			fmt_write_u32(1, cs.mb_rows);
			fmt_write_nl(1);
			fmt_write_str(1, "  MB total:         ");
			fmt_write_u32(1, cs.mb_total);
			fmt_write_nl(1);
			fmt_write_str(1, "  MB skip_coeff:    ");
			fmt_write_u32(1, cs.mb_skip_coeff);
			fmt_write_nl(1);
			fmt_write_str(1, "  MB B_PRED:        ");
			fmt_write_u32(1, cs.mb_b_pred);
			fmt_write_nl(1);
			print_coeff_stats(&cs);
			fmt_write_str(1, "  Ymode DC:         ");
			fmt_write_u32(1, cs.ymode_counts[0]);
			fmt_write_nl(1);
			fmt_write_str(1, "  Ymode V:          ");
			fmt_write_u32(1, cs.ymode_counts[1]);
			fmt_write_nl(1);
			fmt_write_str(1, "  Ymode H:          ");
			fmt_write_u32(1, cs.ymode_counts[2]);
			fmt_write_nl(1);
			fmt_write_str(1, "  Ymode TM:         ");
			fmt_write_u32(1, cs.ymode_counts[3]);
			fmt_write_nl(1);
			fmt_write_str(1, "  Ymode B_PRED:     ");
			fmt_write_u32(1, cs.ymode_counts[4]);
			fmt_write_nl(1);
			fmt_write_str(1, "  UVmode DC:        ");
			fmt_write_u32(1, cs.uv_mode_counts[0]);
			fmt_write_nl(1);
			fmt_write_str(1, "  UVmode V:         ");
			fmt_write_u32(1, cs.uv_mode_counts[1]);
			fmt_write_nl(1);
			fmt_write_str(1, "  UVmode H:         ");
			fmt_write_u32(1, cs.uv_mode_counts[2]);
			fmt_write_nl(1);
			fmt_write_str(1, "  UVmode TM:        ");
			fmt_write_u32(1, cs.uv_mode_counts[3]);
			fmt_write_nl(1);
			fmt_write_str(1, "  Coeff nonzero:    ");
			fmt_write_u32(1, cs.coeff_nonzero_total);
			fmt_write_nl(1);
			fmt_write_str(1, "  Coeff EOB tokens: ");
			fmt_write_u32(1, cs.coeff_eob_tokens);
			fmt_write_nl(1);
			fmt_write_str(1, "  Coeff abs max:    ");
			fmt_write_u32(1, cs.coeff_abs_max);
			fmt_write_nl(1);
			fmt_write_str(1, "  Blocks nonzero Y2:");
			fmt_write_u32(1, cs.blocks_nonzero_y2);
			fmt_write_str(1, " /");
			fmt_write_u32(1, cs.blocks_total_y2);
			fmt_write_nl(1);
			fmt_write_str(1, "  Blocks nonzero Y: ");
			fmt_write_u32(1, cs.blocks_nonzero_y);
			fmt_write_str(1, " /");
			fmt_write_u32(1, cs.blocks_total_y);
			fmt_write_nl(1);
			fmt_write_str(1, "  Blocks nonzero U: ");
			fmt_write_u32(1, cs.blocks_nonzero_u);
			fmt_write_str(1, " /");
			fmt_write_u32(1, cs.blocks_total_u);
			fmt_write_nl(1);
			fmt_write_str(1, "  Blocks nonzero V: ");
			fmt_write_u32(1, cs.blocks_nonzero_v);
			fmt_write_str(1, " /");
			fmt_write_u32(1, cs.blocks_total_v);
			fmt_write_nl(1);
		}
#endif
	}

	os_unmap_file(file);
	return 0;
}

static int cmd_yuv(const char* in_path, const char* out_path) {
	ByteSpan file;
	if (os_map_file_readonly(in_path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	if (vp8_reconstruct_keyframe_yuv(&kf, &decoded, &img) != 0) {
		fmt_write_str(2, "error: VP8 reconstruction failed\n");
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fmt_write_str(2, "error: cannot open output file\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	size_t ysz = (size_t)img.stride_y * (size_t)img.height;
	size_t uvh = (size_t)((img.height + 1u) / 2u);
	size_t uvsz = (size_t)img.stride_uv * uvh;
	int wrc = 0;
	wrc |= os_write_all(fd, img.y, ysz);
	wrc |= os_write_all(fd, img.u, uvsz);
	wrc |= os_write_all(fd, img.v, uvsz);
	(void)close(fd);

	if (wrc != 0) {
		fmt_write_str(2, "error: write failed\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return 0;
}

static int cmd_yuvf(const char* in_path, const char* out_path) {
	ByteSpan file;
	if (os_map_file_readonly(in_path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	if (vp8_reconstruct_keyframe_yuv_filtered(&kf, &decoded, &img) != 0) {
		fmt_write_str(2, "error: VP8 reconstruction/loopfilter failed\n");
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fmt_write_str(2, "error: cannot open output file\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	size_t ysz = (size_t)img.stride_y * (size_t)img.height;
	size_t uvh = (size_t)((img.height + 1u) / 2u);
	size_t uvsz = (size_t)img.stride_uv * uvh;
	int wrc = 0;
	wrc |= os_write_all(fd, img.y, ysz);
	wrc |= os_write_all(fd, img.u, uvsz);
	wrc |= os_write_all(fd, img.v, uvsz);
	(void)close(fd);

	if (wrc != 0) {
		fmt_write_str(2, "error: write failed\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return 0;
}

#ifndef DECODER_TINY

static int cmd_ppm(const char* in_path, const char* out_path) {
	ByteSpan file;
	if (os_map_file_readonly(in_path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	// Match dwebp default output: filtered reconstruction.
	if (vp8_reconstruct_keyframe_yuv_filtered(&kf, &decoded, &img) != 0) {
		fmt_write_str(2, "error: VP8 reconstruction/loopfilter failed\n");
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fmt_write_str(2, "error: cannot open output file\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int wrc = yuv420_write_ppm_fd(fd, &img);
	(void)close(fd);

	if (wrc != 0) {
		fmt_write_str(2, "error: PPM write failed\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return 0;
}

static int cmd_png(const char* in_path, const char* out_path) {
	ByteSpan file;
	if (os_map_file_readonly(in_path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	// Match dwebp default output: filtered reconstruction.
	if (vp8_reconstruct_keyframe_yuv_filtered(&kf, &decoded, &img) != 0) {
		fmt_write_str(2, "error: VP8 reconstruction/loopfilter failed\n");
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fmt_write_str(2, "error: cannot open output file\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int wrc = yuv420_write_png_fd(fd, &img);
	(void)close(fd);

	if (wrc != 0) {
		fmt_write_str(2, "error: PNG write failed\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return 0;
}

static uint64_t u64_abs_diff_u8(uint8_t a, uint8_t b) { return (a >= b) ? (uint64_t)(a - b) : (uint64_t)(b - a); }

static int cmd_diff_mb(const char* webp_path, const char* oracle_i420_path) {
	ByteSpan file;
	if (os_map_file_readonly(webp_path, &file) != 0) {
		fmt_write_str(2, "error: cannot open/map file\n");
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		fmt_write_str(2, "error: not a supported simple lossy WebP (RIFF/WEBP + single VP8 chunk)\n");
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		fmt_write_str(2, "error: VP8 key-frame header parse failed\n");
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		fmt_write_str(2, "error: VP8 macroblock/token decode failed\n");
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	if (vp8_reconstruct_keyframe_yuv(&kf, &decoded, &img) != 0) {
		fmt_write_str(2, "error: VP8 reconstruction failed\n");
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	ByteSpan oracle;
	if (os_map_file_readonly(oracle_i420_path, &oracle) != 0) {
		fmt_write_str(2, "error: cannot open/map oracle i420\n");
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	size_t ysz = (size_t)img.stride_y * (size_t)img.height;
	size_t uvh = (size_t)((img.height + 1u) / 2u);
	size_t uvsz = (size_t)img.stride_uv * uvh;
	size_t expected = ysz + 2u * uvsz;
	if (oracle.size != expected) {
		fmt_write_str(2, "error: oracle size mismatch (expected ");
		fmt_write_size(2, expected);
		fmt_write_str(2, ", got ");
		fmt_write_size(2, oracle.size);
		fmt_write_str(2, ")\n");
		os_unmap_file(oracle);
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	const uint8_t* oy = oracle.data;
	const uint8_t* ou = oracle.data + ysz;
	const uint8_t* ov = oracle.data + ysz + uvsz;

	uint64_t sad_y[4] = {0, 0, 0, 0};
	uint64_t sad_u[4] = {0, 0, 0, 0};
	uint64_t sad_v[4] = {0, 0, 0, 0};
	uint32_t cnt[4] = {0, 0, 0, 0};
	uint64_t sad_y_all = 0, sad_u_all = 0, sad_v_all = 0;

	uint32_t mb_cols = decoded.mb_cols;
	uint32_t mb_rows = decoded.mb_rows;
	uint32_t cw = (img.width + 1u) / 2u;
	uint32_t ch = (img.height + 1u) / 2u;

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb = mb_r * mb_cols + mb_c;
			uint32_t seg = decoded.segmentation_enabled ? (uint32_t)(decoded.segment_id[mb] & 3u) : 0u;
			if (seg > 3u) seg = 0u;
			cnt[seg]++;

			uint32_t x = mb_c * 16u;
			uint32_t y = mb_r * 16u;
			uint32_t xe = x + 16u;
			uint32_t ye = y + 16u;
			if (xe > img.width) xe = img.width;
			if (ye > img.height) ye = img.height;
			for (uint32_t yy = y; yy < ye; yy++) {
				for (uint32_t xx = x; xx < xe; xx++) {
					uint8_t a = img.y[yy * img.stride_y + xx];
					uint8_t b = oy[yy * img.stride_y + xx];
					uint64_t d = u64_abs_diff_u8(a, b);
					sad_y[seg] += d;
					sad_y_all += d;
				}
			}

			uint32_t cx = mb_c * 8u;
			uint32_t cy = mb_r * 8u;
			uint32_t cxe = cx + 8u;
			uint32_t cye = cy + 8u;
			if (cxe > cw) cxe = cw;
			if (cye > ch) cye = ch;
			for (uint32_t yy = cy; yy < cye; yy++) {
				for (uint32_t xx = cx; xx < cxe; xx++) {
					uint8_t au = img.u[yy * img.stride_uv + xx];
					uint8_t bu = ou[yy * img.stride_uv + xx];
					uint8_t av = img.v[yy * img.stride_uv + xx];
					uint8_t bv = ov[yy * img.stride_uv + xx];
					uint64_t du = u64_abs_diff_u8(au, bu);
					uint64_t dv = u64_abs_diff_u8(av, bv);
					sad_u[seg] += du;
					sad_v[seg] += dv;
					sad_u_all += du;
					sad_v_all += dv;
				}
			}
		}
	}

	fmt_write_str(1, "File: ");
	fmt_write_str(1, webp_path);
	fmt_write_nl(1);
	fmt_write_str(1, "Oracle: ");
	fmt_write_str(1, oracle_i420_path);
	fmt_write_nl(1);
	fmt_write_str(1, "Dims: ");
	fmt_write_u32(1, img.width);
	fmt_write_str(1, "x");
	fmt_write_u32(1, img.height);
	fmt_write_nl(1);
	fmt_write_str(1, "Segmentation enabled: ");
	fmt_write_u32(1, decoded.segmentation_enabled);
	fmt_write_nl(1);
	fmt_write_str(1, "Total SAD (Y/U/V): ");
	fmt_write_u64(1, sad_y_all);
	fmt_write_str(1, " /");
	fmt_write_u64(1, sad_u_all);
	fmt_write_str(1, " /");
	fmt_write_u64(1, sad_v_all);
	fmt_write_nl(1);

	for (uint32_t s = 0; s < 4; s++) {
		if (cnt[s] == 0) continue;
		fmt_write_str(1, "  seg ");
		fmt_write_u32(1, s);
		fmt_write_str(1, ": mbs=");
		fmt_write_u32(1, cnt[s]);
		fmt_write_str(1, " sad(Y/U/V)=");
		fmt_write_u64(1, sad_y[s]);
		fmt_write_str(1, "/");
		fmt_write_u64(1, sad_u[s]);
		fmt_write_str(1, "/");
		fmt_write_u64(1, sad_v[s]);
		fmt_write_nl(1);
	}

	os_unmap_file(oracle);
	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return 0;
}

#endif

int main(int argc, char** argv) {
	if (argc < 3) {
		usage();
		return 2;
	}
	if (argv[1][0] == '-' && argv[1][1] == 'i' && argv[1][2] == 'n' && argv[1][3] == 'f' &&
	    argv[1][4] == 'o' && argv[1][5] == '\0') {
		if (argc != 3) {
			usage();
			return 2;
		}
		return cmd_info(argv[2]);
	}

#ifndef DECODER_TINY
	if (argv[1][0] == '-' && argv[1][1] == 'p' && argv[1][2] == 'r' && argv[1][3] == 'o' &&
	    argv[1][4] == 'b' && argv[1][5] == 'e' && argv[1][6] == '\0') {
		if (argc != 3) {
			usage();
			return 2;
		}
		return cmd_probe(argv[2]);
	}
	if (argv[1][0] == '-' && argv[1][1] == 'd' && argv[1][2] == 'u' && argv[1][3] == 'm' &&
	    argv[1][4] == 'p' && argv[1][5] == '_' && argv[1][6] == 'm' && argv[1][7] == 'b' &&
	    argv[1][8] == '\0') {
		uint32_t mb_index = 0;
		if (argc == 4) {
			mb_index = (uint32_t)strtoul(argv[3], NULL, 10);
		} else if (argc != 3) {
			usage();
			return 2;
		}
		return cmd_dump_mb(argv[2], mb_index);
	}
#endif
	if (argv[1][0] == '-' && argv[1][1] == 'y' && argv[1][2] == 'u' && argv[1][3] == 'v' && argv[1][4] == '\0') {
		if (argc != 4) {
			usage();
			return 2;
		}
		return cmd_yuv(argv[2], argv[3]);
	}
	if (argv[1][0] == '-' && argv[1][1] == 'y' && argv[1][2] == 'u' && argv[1][3] == 'v' && argv[1][4] == 'f' &&
	    argv[1][5] == '\0') {
		if (argc != 4) {
			usage();
			return 2;
		}
		return cmd_yuvf(argv[2], argv[3]);
	}

#ifndef DECODER_TINY
	if (argv[1][0] == '-' && argv[1][1] == 'p' && argv[1][2] == 'p' && argv[1][3] == 'm' && argv[1][4] == '\0') {
		if (argc != 4) {
			usage();
			return 2;
		}
		return cmd_ppm(argv[2], argv[3]);
	}
	if (argv[1][0] == '-' && argv[1][1] == 'p' && argv[1][2] == 'n' && argv[1][3] == 'g' && argv[1][4] == '\0') {
		if (argc != 4) {
			usage();
			return 2;
		}
		return cmd_png(argv[2], argv[3]);
	}
	if (argv[1][0] == '-' && argv[1][1] == 'd' && argv[1][2] == 'i' && argv[1][3] == 'f' && argv[1][4] == 'f' &&
	    argv[1][5] == '_' && argv[1][6] == 'm' && argv[1][7] == 'b' && argv[1][8] == '\0') {
		if (argc != 4) {
			usage();
			return 2;
		}
		return cmd_diff_mb(argv[2], argv[3]);
	}
#endif
	(void)errno;
	usage();
	return 2;
}
