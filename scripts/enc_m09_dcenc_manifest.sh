#!/usr/bin/env sh
set -eu

# Generates a deterministic manifest of decoded RGB bytes for our encoder outputs.
# For each input and quality, encode -> decode via our decoder -> hash raw RGB.
# Output format (sorted):
#   images/png-in/foo.png  q=<quality>  <sha256>

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

make -s all enc_pngdump enc_m09_dcenc >/dev/null

QUALS="0 10 50 75 90 100"
TMP_WEBP=build/enc_m09_dcenc_tmp.webp
TMP_PNG=build/enc_m09_dcenc_tmp.png
trap 'rm -f "$TMP_WEBP" "$TMP_PNG"' EXIT

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1
		for q in $QUALS; do
			./build/enc_m09_dcenc --q "$q" "$f" "$TMP_WEBP" 2>/dev/null
			./decoder -png "$TMP_WEBP" "$TMP_PNG" 2>/dev/null
			h=$(./build/enc_pngdump --rgb "$TMP_PNG" - 2>/dev/null | sha256sum | awk '{print $1}')
			printf '%s  q=%s  %s\n' "$f" "$q" "$h"
		done
	done
	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort
