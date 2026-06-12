#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

BA_BIN="$ROOT_DIR/butteraugli_nolibc_png"
if [ ! -x "$BA_BIN" ]; then
	echo "error: missing executable: $BA_BIN" >&2
	echo "hint: provide butteraugli_nolibc_png in repo root" >&2
	exit 2
fi

make -s all >/dev/null

tmpdir=$(mk_artifact_tmpdir)

q=75
mode=bpred-rdo
token_probs=adaptive

score_png_pair() {
	ref_png=$1
	out_png=$2
	raw="$($BA_BIN "$ref_png" "$out_png" 2>/dev/null || true)"
	score=$(printf '%s\n' "$raw" | awk 'match($0, /[0-9]+(\.[0-9]+)?/){print substr($0, RSTART, RLENGTH); exit}')
	if [ -z "$score" ]; then
		score=nan
	fi
	printf '%s' "$score"
}

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1

		base=$(basename "$f" .png)
		out_webp="$tmpdir/$base.out.webp"
		out_png="$tmpdir/$base.out.png"

		./build/encoder --q "$q" --mode "$mode" --loopfilter --token-probs "$token_probs" "$f" "$out_webp" >/dev/null 2>&1
		./build/decoder -png "$out_webp" "$out_png" >/dev/null 2>&1

		bytes=$(wc -c < "$out_webp" | tr -d ' ')
		score=$(score_png_pair "$f" "$out_png")

		printf '%s  bytes=%s butteraugli=%s\n' "$f" "$bytes" "$score"
		done

	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort