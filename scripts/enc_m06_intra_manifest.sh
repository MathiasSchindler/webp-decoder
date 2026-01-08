#!/usr/bin/env sh
set -eu

# Generates a deterministic manifest of DC intra forward-transform coefficient dumps.
# Output format (sorted):
#   images/png-in/foo.png  <sha256>

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

make -s enc_m06_intradump >/dev/null

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1
		h=$(./build/enc_m06_intradump "$f" - 2>/dev/null | sha256sum | awk '{print $1}')
		printf '%s  %s\n' "$f" "$h"
	done
	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort
