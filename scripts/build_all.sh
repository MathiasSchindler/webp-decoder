#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# Convenience helper for local dev/CI to build all expected binaries.
# Keep it simple: delegate to Makefile targets.

echo "== build: all ==" >&2
make -s all

echo "== build: nolibc ==" >&2
make -s nolibc

echo "== build: nolibc_tiny ==" >&2
make -s nolibc_tiny

echo "== build: ultra ==" >&2
make -s ultra

echo "OK: built all binaries" >&2
