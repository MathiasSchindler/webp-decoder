#!/usr/bin/env sh
set -eu

# Generates a deterministic manifest of quantized DC-intra coefficient dumps.
# Output format (sorted):
#   images/png-in/foo.png  q=<quality>  <sha256>

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

make -s enc_m07_quantdump >/dev/null

QUALS="0 10 50 75 90 100"

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1
		for q in $QUALS; do
			h=$(./build/enc_m07_quantdump --q "$q" "$f" - 2>/dev/null | sha256sum | awk '{print $1}')
			printf '%s  q=%s  %s\n' "$f" "$q" "$h"
		done
	done
	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort
