#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

tmpdir=$(mk_artifact_tmpdir)
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

enc_normal="$ROOT_DIR/encoder"
enc_ultra="$ROOT_DIR/encoder_nolibc_ultra"

if [[ ! -x "$enc_normal" ]]; then
	die "missing $enc_normal (run 'make' first)"
fi

if [[ ! -x "$enc_ultra" ]]; then
	note "encoder_nolibc_ultra not found; building via 'make ultra'"
	( cd "$ROOT_DIR" && make ultra >/dev/null )
fi

# If the normal encoder was rebuilt more recently, ensure the ultra binary isn't stale.
if [[ -x "$enc_ultra" && "$enc_normal" -nt "$enc_ultra" ]]; then
	note "encoder_nolibc_ultra is stale; rebuilding via 'make ultra'"
	( cd "$ROOT_DIR" && make ultra >/dev/null )
fi

if [[ ! -x "$enc_ultra" ]]; then
	die "missing $enc_ultra (build failed?)"
fi

# Use the same deterministic corpus as the encoder m05 YUV gate.
corpus_file="$ROOT_DIR/scripts/enc_m05_yuv_expected.txt"
if [[ ! -f "$corpus_file" ]]; then
	die "missing corpus list: $corpus_file"
fi

quality=75
mode=bpred

failures=0
count=0

while read -r png _hash; do
	[[ -z "$png" ]] && continue
	[[ "$png" == \#* ]] && continue

	count=$((count + 1))
	out_a="$tmpdir/normal_$count.webp"
	out_b="$tmpdir/ultra_$count.webp"

	"$enc_normal" --q "$quality" --mode "$mode" --token-probs default "$ROOT_DIR/$png" "$out_a" >/dev/null
	"$enc_ultra" "$ROOT_DIR/$png" "$out_b" >/dev/null

	if ! cmp -s "$out_a" "$out_b"; then
		h_a=$(sha256sum "$out_a" | awk '{print $1}')
		h_b=$(sha256sum "$out_b" | awk '{print $1}')
		note "mismatch: $png"
		note "  encoder            sha256=$h_a"
		note "  encoder_nolibc_ultra sha256=$h_b"
		failures=$((failures + 1))
		break
	fi
done < "$corpus_file"

if [[ $failures -ne 0 ]]; then
	exit 1
fi

note "OK: encoder_nolibc_ultra matches encoder (q=$quality mode=$mode) for $count images"
