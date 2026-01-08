#!/usr/bin/env sh
set -eu

# Generates a deterministic manifest of:
# - decoded RGB bytes for our encoder outputs (encode -> decode via our decoder)
# - the raw intra mode maps dumped by the encoder
#
# Output format (sorted):
#   images/png-in/foo.png  q=<quality>  rgb=<sha256>  modes=<sha256>

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

make -s all enc_pngdump enc_m09_bpredenc >/dev/null

QUALS="0 10 50 75 90 100"
TMP_WEBP=build/enc_m09_bpredenc_tmp.webp
TMP_PNG=build/enc_m09_bpredenc_tmp.png
TMP_MODES=build/enc_m09_bpredenc_tmp.modes
trap 'rm -f "$TMP_WEBP" "$TMP_PNG" "$TMP_MODES"' EXIT

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1
		for q in $QUALS; do
			./build/enc_m09_bpredenc --q "$q" --dump-modes "$TMP_MODES" "$f" "$TMP_WEBP" 2>/dev/null
			./decoder -png "$TMP_WEBP" "$TMP_PNG" 2>/dev/null
			rgb=$(./build/enc_pngdump --rgb "$TMP_PNG" - 2>/dev/null | sha256sum | awk '{print $1}')
			modes=$(sha256sum "$TMP_MODES" | awk '{print $1}')
			printf '%s  q=%s  rgb=%s  modes=%s\n' "$f" "$q" "$rgb" "$modes"
		done
	done
	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort
