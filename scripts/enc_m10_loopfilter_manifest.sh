#!/usr/bin/env sh
set -eu

# Generates a deterministic manifest of the loopfilter-related header fields
# (as reported by libwebp webpinfo -bitstream_info) for our encoder output.
#
# Output format (sorted):
#   <png>  q=<quality>  baseq=<qindex>  simple=<0|1>  level=<0..63>  sharpness=<0..7>  lfdelta=<0|1>

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

. "$ROOT_DIR/scripts/common.sh"
require_libwebp_webpinfo

make -s all enc_m09_dcenc >/dev/null

# Keep the gate fast: pick a single deterministic input.
# Loopfilter params depend only on qindex, so this still validates the header wiring.
IN_PNG=$(ls images/png-in/*.png 2>/dev/null | LC_ALL=C sort | head -n 1 || true)
if [ -z "$IN_PNG" ] || [ ! -f "$IN_PNG" ]; then
	echo "error: no PNGs found under images/png-in" >&2
	exit 2
fi

QUALS="0 10 50 75 90 100"
TMP_WEBP=build/enc_m10_loopfilter_tmp.webp
trap 'rm -f "$TMP_WEBP"' EXIT

{
	for q in $QUALS; do
		./build/enc_m09_dcenc --q "$q" --loopfilter "$IN_PNG" "$TMP_WEBP" 2>/dev/null
		info=$("$WEBPINFO" -bitstream_info "$TMP_WEBP" 2>/dev/null)

		simple=$(printf '%s\n' "$info" | awk -F: '/^  Simple filter:/{gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
		level=$(printf '%s\n' "$info" | awk -F: '/^  Level:/{gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
		sharp=$(printf '%s\n' "$info" | awk -F: '/^  Sharpness:/{gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
		lfdelta=$(printf '%s\n' "$info" | awk -F: '/^  Use lf delta:/{gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
		baseq=$(printf '%s\n' "$info" | awk -F: '/^  Base Q:/{gsub(/^[[:space:]]+/, "", $2); print $2; exit}')

		if [ -z "${simple:-}" ] || [ -z "${level:-}" ] || [ -z "${sharp:-}" ] || [ -z "${lfdelta:-}" ] || [ -z "${baseq:-}" ]; then
			echo "error: failed to extract loopfilter fields from webpinfo output" >&2
			exit 1
		fi

		printf '%s  q=%s  baseq=%s  simple=%s  level=%s  sharpness=%s  lfdelta=%s\n' \
			"$IN_PNG" "$q" "$baseq" "$simple" "$level" "$sharp" "$lfdelta"
	done
} | LC_ALL=C sort
