#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

need() {
  local f="$1"
  if [[ ! -x "$f" ]]; then
    echo "error: missing executable: $f (run 'make' first)" >&2
    exit 2
  fi
}

need ./decoder
need ./encoder

echo "OK: required binaries present (decoder, encoder)"
