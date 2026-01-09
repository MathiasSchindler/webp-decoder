#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC1091
. "$ROOT_DIR/scripts/common.sh"

resolve_libwebp_tools

missing=0
if [[ -z "${WEBPINFO:-}" || ! -x "$WEBPINFO" ]]; then
  echo "MISSING: webpinfo (set LIBWEBP_BIN_DIR)" >&2
  missing=1
fi
if [[ -z "${DWEBP:-}" || ! -x "$DWEBP" ]]; then
  echo "MISSING: dwebp (set LIBWEBP_BIN_DIR)" >&2
  missing=1
fi

if [[ $missing -ne 0 ]]; then
  exit 2
fi

echo "OK: oracle tools found"
echo "  WEBPINFO=$WEBPINFO"
echo "  DWEBP=$DWEBP"
if [[ -n "${CWEBP:-}" && -x "$CWEBP" ]]; then
  echo "  CWEBP=$CWEBP"
fi
