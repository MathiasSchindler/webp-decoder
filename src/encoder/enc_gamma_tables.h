#ifndef ENC_GAMMA_TABLES_H
#define ENC_GAMMA_TABLES_H

#include <stdint.h>

enum {
	ENC_GAMMA_FIX = 12,
	ENC_GAMMA_TAB_FIX = 7,
	ENC_GAMMA_TAB_SIZE = 1 << (ENC_GAMMA_FIX - ENC_GAMMA_TAB_FIX),
};

extern const uint16_t enc_gamma_to_linear_tab[256];
extern const int enc_linear_to_gamma_tab[ENC_GAMMA_TAB_SIZE + 1];

#endif
