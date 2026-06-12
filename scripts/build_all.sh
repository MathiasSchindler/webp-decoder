#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# Convenience helper for local dev/CI to build all expected binaries.
# Keep it simple: delegate to Makefile targets.

echo "== build: nolibc decoder+encoder ==" >&2
make -s

echo "OK: built all binaries" >&2
