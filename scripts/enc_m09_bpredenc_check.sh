#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

TMP=build/enc_m09_bpredenc_manifest_actual.txt
trap 'rm -f "$TMP"' EXIT

./scripts/enc_m09_bpredenc_manifest.sh >"$TMP"

diff -u scripts/enc_m09_bpredenc_expected.txt "$TMP"

echo "OK: encoder m09 B_PRED round-trip matches expected manifest"
