#include "vp8_tree.h"

int vp8_treed_read(BoolDecoder* d, const int8_t* tree, const uint8_t* probs, int start_node) {
	int node = start_node;
	for (;;) {
		int8_t left = tree[node + 0];
		int8_t right = tree[node + 1];
		uint8_t p = probs[(unsigned)node >> 1];
		int bit = bool_decode_bool(d, p);
		int next = bit ? (int)right : (int)left;
		if (next <= 0) {
			return -next;
		}
		node = next;
	}
}
