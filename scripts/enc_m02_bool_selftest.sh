#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

make -s enc_boolselftest

./build/enc_boolselftest

echo "OK: encoder m02 bool encoder round-trip" >&2
