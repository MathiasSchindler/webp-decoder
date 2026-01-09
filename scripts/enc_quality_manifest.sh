#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

require_libwebp_dwebp

make -s enc_png2ppm enc_quality_metrics >/dev/null

tmpdir=$(mk_artifact_tmpdir)

q=75
mode=bpred
lf_flag=
if [ "${LOOPFILTER:-0}" = "1" ]; then
	lf_flag="--loopfilter"
fi

{
	found=0
	for f in images/png-in/*.png; do
		if [ ! -f "$f" ]; then
			continue
		fi
		found=1

		base=$(basename "$f" .png)
		ref_ppm="$tmpdir/$base.ref.ppm"
		out_webp="$tmpdir/$base.out.webp"
		out_ppm="$tmpdir/$base.out.ppm"

		./build/enc_png2ppm "$f" "$ref_ppm" >/dev/null 2>&1
		./encoder --q "$q" --mode "$mode" $lf_flag "$f" "$out_webp" >/dev/null 2>&1
		"$DWEBP" -quiet "$out_webp" -ppm -o "$out_ppm" >/dev/null 2>&1

		metrics=$(./build/enc_quality_metrics "$ref_ppm" "$out_ppm") || exit 1
		printf '%s  %s\n' "$f" "$metrics"
	done

	if [ "$found" -eq 0 ]; then
		echo "No PNGs found under images/png-in" >&2
		exit 1
	fi
} | LC_ALL=C sort
