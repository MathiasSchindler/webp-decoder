#include "vp8_loopfilter.h"

#include <errno.h>
#include <stddef.h>

static inline int iabs_i32(int v) { return (v < 0) ? -v : v; }

static inline int clamp_i8(int v) {
	if (v < -128) return -128;
	if (v > 127) return 127;
	return v;
}

static inline uint8_t clamp_u8(int v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

static inline int p_at(const uint8_t* q0, int step, int k) {
	return (int)q0[k * step];
}

static int high_edge_variance(const uint8_t* q0, int step, int hev_threshold) {
	int p1 = p_at(q0, step, -2);
	int p0 = p_at(q0, step, -1);
	int q0v = p_at(q0, step, 0);
	int q1 = p_at(q0, step, 1);
	return iabs_i32(p1 - p0) > hev_threshold || iabs_i32(q1 - q0v) > hev_threshold;
}

static int simple_threshold(const uint8_t* q0, int step, int filter_limit) {
	int p1 = p_at(q0, step, -2);
	int p0 = p_at(q0, step, -1);
	int q0v = p_at(q0, step, 0);
	int q1 = p_at(q0, step, 1);
	return (iabs_i32(p0 - q0v) * 2 + (iabs_i32(p1 - q1) >> 1)) <= filter_limit;
}

static int normal_threshold(const uint8_t* q0, int step, int edge_limit, int interior_limit) {
	int I = interior_limit;
	int E = edge_limit;
	if (!simple_threshold(q0, step, 2 * E + I)) return 0;

	int p3 = p_at(q0, step, -4);
	int p2 = p_at(q0, step, -3);
	int p1 = p_at(q0, step, -2);
	int p0 = p_at(q0, step, -1);
	int q0v = p_at(q0, step, 0);
	int q1 = p_at(q0, step, 1);
	int q2 = p_at(q0, step, 2);
	int q3 = p_at(q0, step, 3);

	return iabs_i32(p3 - p2) <= I && iabs_i32(p2 - p1) <= I && iabs_i32(p1 - p0) <= I && iabs_i32(q3 - q2) <= I &&
	       iabs_i32(q2 - q1) <= I && iabs_i32(q1 - q0v) <= I;
}

static void filter_common(uint8_t* q0, int step, int use_outer_taps) {
	int p1 = p_at(q0, step, -2);
	int p0 = p_at(q0, step, -1);
	int q0v = p_at(q0, step, 0);
	int q1 = p_at(q0, step, 1);

	int a = 3 * (q0v - p0);
	if (use_outer_taps) a += clamp_i8(p1 - q1);
	a = clamp_i8(a);

	int f1 = clamp_i8(a + 4) >> 3;
	int f2 = clamp_i8(a + 3) >> 3;

	q0[0] = clamp_u8(q0v - f1);
	q0[-step] = clamp_u8(p0 + f2);

	if (!use_outer_taps) {
		int a2 = (f1 + 1) >> 1;
		q0[step] = clamp_u8(q1 - a2);
		q0[-2 * step] = clamp_u8(p1 + a2);
	}
}

static void filter_mb_edge(uint8_t* q0, int step) {
	int p2 = p_at(q0, step, -3);
	int p1 = p_at(q0, step, -2);
	int p0 = p_at(q0, step, -1);
	int q0v = p_at(q0, step, 0);
	int q1 = p_at(q0, step, 1);
	int q2 = p_at(q0, step, 2);
	int q3 = p_at(q0, step, 3);

	(void)q3;
	int w = clamp_i8(clamp_i8(p1 - q1) + 3 * (q0v - p0));

	int a = (27 * w + 63) >> 7;
	q0[-step] = clamp_u8(p0 + a);
	q0[0] = clamp_u8(q0v - a);

	a = (18 * w + 63) >> 7;
	q0[-2 * step] = clamp_u8(p1 + a);
	q0[step] = clamp_u8(q1 - a);

	a = (9 * w + 63) >> 7;
	q0[-3 * step] = clamp_u8(p2 + a);
	q0[2 * step] = clamp_u8(q2 - a);
}

static void filter_mb_v_edge(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold,
                            int size_blocks) {
	for (int i = 0; i < 8 * size_blocks; i++) {
		if (normal_threshold(src_q0, 1, edge_limit, interior_limit)) {
			if (high_edge_variance(src_q0, 1, hev_threshold))
				filter_common(src_q0, 1, 1);
			else
				filter_mb_edge(src_q0, 1);
		}
		src_q0 += stride;
	}
}

static void filter_subblock_v_edge(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold,
                                  int size_blocks) {
	for (int i = 0; i < 8 * size_blocks; i++) {
		if (normal_threshold(src_q0, 1, edge_limit, interior_limit)) {
			filter_common(src_q0, 1, high_edge_variance(src_q0, 1, hev_threshold));
		}
		src_q0 += stride;
	}
}

static void filter_mb_h_edge(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold,
                            int size_blocks) {
	for (int i = 0; i < 8 * size_blocks; i++) {
		if (normal_threshold(src_q0, stride, edge_limit, interior_limit)) {
			if (high_edge_variance(src_q0, stride, hev_threshold))
				filter_common(src_q0, stride, 1);
			else
				filter_mb_edge(src_q0, stride);
		}
		src_q0 += 1;
	}
}

static void filter_subblock_h_edge(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold,
                                  int size_blocks) {
	for (int i = 0; i < 8 * size_blocks; i++) {
		if (normal_threshold(src_q0, stride, edge_limit, interior_limit)) {
			filter_common(src_q0, stride, high_edge_variance(src_q0, stride, hev_threshold));
		}
		src_q0 += 1;
	}
}

static void filter_v_edge_simple(uint8_t* src_q0, int stride, int filter_limit) {
	for (int i = 0; i < 16; i++) {
		if (simple_threshold(src_q0, 1, filter_limit)) filter_common(src_q0, 1, 1);
		src_q0 += stride;
	}
}

static void filter_h_edge_simple(uint8_t* src_q0, int stride, int filter_limit) {
	for (int i = 0; i < 16; i++) {
		if (simple_threshold(src_q0, stride, filter_limit)) filter_common(src_q0, stride, 1);
		src_q0 += 1;
	}
}

static void calc_params_keyframe(const Vp8DecodedFrame* decoded, uint32_t mb, int* edge_limit, int* interior_limit,
                                 int* hev_threshold) {
	int level = (int)decoded->lf_level;
	if (decoded->segmentation_enabled) {
		uint32_t seg = (uint32_t)(decoded->segment_id[mb] & 3u);
		int seg_adj = (int)decoded->seg_lf_level[seg];
		level = decoded->segmentation_abs ? seg_adj : (level + seg_adj);
	}
	if (level < 0) level = 0;
	if (level > 63) level = 63;

	if (decoded->lf_delta_enabled) {
		level += (int)decoded->lf_ref_delta[0];
		if (decoded->ymode[mb] == 4) level += (int)decoded->lf_mode_delta[0];
		if (level < 0) level = 0;
		if (level > 63) level = 63;
	}

	int ilim = level;
	if (decoded->lf_sharpness) {
		int sh = (int)decoded->lf_sharpness;
		ilim >>= (sh > 4) ? 2 : 1;
		int cap = 9 - sh;
		if (ilim > cap) ilim = cap;
	}
	if (ilim < 1) ilim = 1;

	int hev = (level >= 15) ? 1 : 0;
	if (level >= 40) hev++;

	*edge_limit = level;
	*interior_limit = ilim;
	*hev_threshold = hev;
}

int vp8_loopfilter_apply_keyframe(Yuv420Image* padded_img, const Vp8DecodedFrame* decoded) {
	if (!padded_img || !decoded) {
		errno = EINVAL;
		return -1;
	}
	if (padded_img->width != decoded->mb_cols * 16u || padded_img->height != decoded->mb_rows * 16u) {
		errno = EINVAL;
		return -1;
	}

	uint32_t mb_cols = decoded->mb_cols;
	uint32_t mb_rows = decoded->mb_rows;

	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb = mb_r * mb_cols + mb_c;

			int edge_limit = 0, interior_limit = 0, hev_threshold = 0;
			calc_params_keyframe(decoded, mb, &edge_limit, &interior_limit, &hev_threshold);
			if (edge_limit == 0) continue;

			uint8_t* y = padded_img->y + (size_t)mb_r * 16u * padded_img->stride_y + (size_t)mb_c * 16u;
			uint8_t* u = padded_img->u + (size_t)mb_r * 8u * padded_img->stride_uv + (size_t)mb_c * 8u;
			uint8_t* v = padded_img->v + (size_t)mb_r * 8u * padded_img->stride_uv + (size_t)mb_c * 8u;

			int filter_subblocks = (decoded->has_coeff && decoded->has_coeff[mb]) || decoded->ymode[mb] == 4;

			if (decoded->lf_use_simple) {
				int mb_limit = (edge_limit + 2) * 2 + interior_limit;
				int b_limit = edge_limit * 2 + interior_limit;

				if (mb_c) filter_v_edge_simple(y, (int)padded_img->stride_y, mb_limit);
				if (filter_subblocks) {
					filter_v_edge_simple(y + 4, (int)padded_img->stride_y, b_limit);
					filter_v_edge_simple(y + 8, (int)padded_img->stride_y, b_limit);
					filter_v_edge_simple(y + 12, (int)padded_img->stride_y, b_limit);
				}

				if (mb_r) filter_h_edge_simple(y, (int)padded_img->stride_y, mb_limit);
				if (filter_subblocks) {
					filter_h_edge_simple(y + 4 * padded_img->stride_y, (int)padded_img->stride_y, b_limit);
					filter_h_edge_simple(y + 8 * padded_img->stride_y, (int)padded_img->stride_y, b_limit);
					filter_h_edge_simple(y + 12 * padded_img->stride_y, (int)padded_img->stride_y, b_limit);
				}
			} else {
				if (mb_c) {
					filter_mb_v_edge(y, (int)padded_img->stride_y, edge_limit + 2, interior_limit, hev_threshold, 2);
					filter_mb_v_edge(u, (int)padded_img->stride_uv, edge_limit + 2, interior_limit, hev_threshold, 1);
					filter_mb_v_edge(v, (int)padded_img->stride_uv, edge_limit + 2, interior_limit, hev_threshold, 1);
				}

				if (filter_subblocks) {
					filter_subblock_v_edge(y + 4, (int)padded_img->stride_y, edge_limit, interior_limit, hev_threshold, 2);
					filter_subblock_v_edge(y + 8, (int)padded_img->stride_y, edge_limit, interior_limit, hev_threshold, 2);
					filter_subblock_v_edge(y + 12, (int)padded_img->stride_y, edge_limit, interior_limit, hev_threshold, 2);
					filter_subblock_v_edge(u + 4, (int)padded_img->stride_uv, edge_limit, interior_limit, hev_threshold, 1);
					filter_subblock_v_edge(v + 4, (int)padded_img->stride_uv, edge_limit, interior_limit, hev_threshold, 1);
				}

				if (mb_r) {
					filter_mb_h_edge(y, (int)padded_img->stride_y, edge_limit + 2, interior_limit, hev_threshold, 2);
					filter_mb_h_edge(u, (int)padded_img->stride_uv, edge_limit + 2, interior_limit, hev_threshold, 1);
					filter_mb_h_edge(v, (int)padded_img->stride_uv, edge_limit + 2, interior_limit, hev_threshold, 1);
				}

				if (filter_subblocks) {
					filter_subblock_h_edge(y + 4 * padded_img->stride_y, (int)padded_img->stride_y, edge_limit, interior_limit,
					                       hev_threshold, 2);
					filter_subblock_h_edge(y + 8 * padded_img->stride_y, (int)padded_img->stride_y, edge_limit, interior_limit,
					                       hev_threshold, 2);
					filter_subblock_h_edge(y + 12 * padded_img->stride_y, (int)padded_img->stride_y, edge_limit, interior_limit,
					                       hev_threshold, 2);
					filter_subblock_h_edge(u + 4 * padded_img->stride_uv, (int)padded_img->stride_uv, edge_limit, interior_limit,
					                       hev_threshold, 1);
					filter_subblock_h_edge(v + 4 * padded_img->stride_uv, (int)padded_img->stride_uv, edge_limit, interior_limit,
					                       hev_threshold, 1);
				}
			}
		}
	}

	return 0;
}
