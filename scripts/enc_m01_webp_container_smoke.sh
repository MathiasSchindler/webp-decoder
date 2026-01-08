#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

WEBPINFO=../../libwebp/examples/webpinfo

if [ ! -x "$WEBPINFO" ]; then
	echo "error: $WEBPINFO not found or not executable" >&2
	exit 2
fi

make -s enc_webpwrap >/dev/null

tmp_dir=${TMPDIR:-/tmp}/webp_encoder_m1_container_smoke
mkdir -p "$tmp_dir"

count=0
fail=0

for in_webp in images/webp/*.webp; do
	[ -e "$in_webp" ] || continue
	count=$((count+1))

	base=$(basename "$in_webp" .webp)
	out_webp="$tmp_dir/${base}.wrapped.webp"

	if ! ./build/enc_webpwrap "$in_webp" "$out_webp" >/dev/null 2>&1; then
		echo "FAIL: enc_webpwrap failed: $in_webp" >&2
		fail=1
		continue
	fi

	# 1) webpinfo must parse
	if ! "$WEBPINFO" "$out_webp" >/dev/null 2>&1; then
		echo "FAIL: webpinfo failed: $out_webp" >&2
		fail=1
		continue
	fi

	# 2) Exactly one VP8 chunk
	vp8_count=$("$WEBPINFO" "$out_webp" 2>/dev/null | awk '/^Chunk VP8 /{c++} END{print c+0}')
	if [ "$vp8_count" -ne 1 ]; then
		echo "FAIL: expected 1 VP8 chunk, got $vp8_count: $out_webp" >&2
		fail=1
		continue
	fi

done

if [ "$count" -eq 0 ]; then
	echo "error: no files matched images/webp/*.webp" >&2
	exit 2
fi

if [ "$fail" -ne 0 ]; then
	echo "FAIL: one or more container smoke tests failed" >&2
	exit 1
fi

echo "OK: $count wrapped files parsed by webpinfo (single VP8 chunk)" >&2
