#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

EXPECTED=scripts/enc_m09_modeenc_expected.txt
ACTUAL=build/enc_m09_modeenc_manifest_actual.txt

./scripts/enc_m09_modeenc_manifest.sh >"$ACTUAL"

diff -u "$EXPECTED" "$ACTUAL"

echo "OK"
