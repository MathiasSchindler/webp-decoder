#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

EXPECTED=scripts/enc_m06_intra_expected.txt
TMP=build/enc_m06_intra_manifest.tmp

if [ "${1:-}" = "--update" ]; then
	scripts/enc_m06_intra_manifest.sh > "$EXPECTED"
	echo "Updated $EXPECTED" >&2
	exit 0
fi

if [ ! -f "$EXPECTED" ]; then
	echo "Missing $EXPECTED" >&2
	echo "Run: scripts/enc_m06_intra_check.sh --update" >&2
	exit 2
fi

scripts/enc_m06_intra_manifest.sh > "$TMP"

diff -u "$EXPECTED" "$TMP"

echo "OK: encoder m06 intra+ftransform matches expected manifest" >&2
