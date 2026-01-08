#pragma once

#include <stdint.h>

#include "../m03_bool_decoder/bool_decoder.h"

// VP8 trees store either a node index (positive, even) or a leaf symbol
// (negative; symbol is -value).
//
// Probabilities are stored in an array indexed by (node_index >> 1).
int vp8_treed_read(BoolDecoder* d, const int8_t* tree, const uint8_t* probs, int start_node);
