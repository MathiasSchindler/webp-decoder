#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

expected="scripts/enc_m10_loopfilter_expected.txt"
actual="build/enc_m10_loopfilter_manifest_actual.txt"

bash scripts/enc_m10_loopfilter_manifest.sh >"$actual"

if diff -u "$expected" "$actual" >/dev/null; then
	echo "OK: encoder m10 loopfilter header fields match expected manifest" >&2
	exit 0
fi

echo "FAIL: encoder m10 loopfilter header fields differ" >&2

diff -u "$expected" "$actual" | head -n 120 >&2 || true

echo "(full manifest at $actual)" >&2
exit 1
