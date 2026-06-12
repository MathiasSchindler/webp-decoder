#pragma once

#include <stdint.h>

typedef struct {
	int qindex;
	int y1_dc;
	int y1_ac;
	int y2_dc;
	int y2_ac;
	int uv_dc;
	int uv_ac;
} Vp8QuantFactors;

int vp8_clamp_q(int q);
int vp8_dc_q(int q);
int vp8_ac_q(int q);

void vp8_quant_factors_from_qindex(int qindex,
                                  int y1_dc_delta,
                                  int y2_dc_delta,
                                  int y2_ac_delta,
                                  int uv_dc_delta,
                                  int uv_ac_delta,
                                  Vp8QuantFactors* out);
