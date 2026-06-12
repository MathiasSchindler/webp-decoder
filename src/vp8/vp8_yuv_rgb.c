#include "vp8_yuv_rgb.h"
#include "vp8_yuv_rgb_x86.h"

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

void vp8_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* dst3) {
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

static void vp8_upsample_rgb_line_only(const uint8_t* top_y,
                                       const uint8_t* top_u,
                                       const uint8_t* top_v,
                                       const uint8_t* cur_u,
                                       const uint8_t* cur_v,
                                       uint8_t* top_dst,
                                       uint32_t len) {
#if defined(VP8_YUV_RGB_HAVE_SSE2)
	vp8_upsample_rgb_line_sse2(top_y, top_u, top_v, cur_u, cur_v, top_dst, len);
	return;
#endif
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
	}
}

static void vp8_upsample_rgb_line_pair_full(const uint8_t* top_y,
                                            const uint8_t* bottom_y,
                                            const uint8_t* top_u,
                                            const uint8_t* top_v,
                                            const uint8_t* cur_u,
                                            const uint8_t* cur_v,
                                            uint8_t* top_dst,
                                            uint8_t* bottom_dst,
                                            uint32_t len) {
#if defined(VP8_YUV_RGB_HAVE_SSE2)
	vp8_upsample_rgb_line_pair_sse2(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_dst, bottom_dst, len);
	return;
#endif
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
	{
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
		{
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
		{
			const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
			vp8_yuv_to_rgb(bottom_y[idx], u0, v0, bottom_dst + idx * 3u);
		}
	}
}

void vp8_upsample_rgb_line_pair(const uint8_t* top_y,
                                const uint8_t* bottom_y,
                                const uint8_t* top_u,
                                const uint8_t* top_v,
                                const uint8_t* cur_u,
                                const uint8_t* cur_v,
                                uint8_t* top_dst,
                                uint8_t* bottom_dst,
                                uint32_t len) {
	if (bottom_y != 0) {
		vp8_upsample_rgb_line_pair_full(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_dst, bottom_dst, len);
	} else {
		vp8_upsample_rgb_line_only(top_y, top_u, top_v, cur_u, cur_v, top_dst, len);
	}
}
