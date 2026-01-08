#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

update=0
if [[ "${1:-}" == "--update" ]]; then
  update=1
  shift
fi

cd "$root_dir"

make -s decoder enc_pngdump enc_m03_miniframe

out_webp="build/enc_m03_miniframe.webp"
out_png="build/enc_m03_miniframe.png"
out_rgb="build/enc_m03_miniframe.rgb"
expected_file="scripts/enc_m03_miniframe_expected.sha256"

rm -f "$out_webp" "$out_png" "$out_rgb"

./build/enc_m03_miniframe "$out_webp"

# Must decode (macroblocks/tokens).
./decoder -probe "$out_webp" >/dev/null

# Produce a PNG and re-parse it with our encoder-side PNG reader.
./decoder -png "$out_webp" "$out_png"
./build/enc_pngdump "$out_png" > "$out_rgb"

# Sanity: 16*16 RGB bytes.
bytes="$(wc -c < "$out_rgb" | tr -d ' ')"
if [[ "$bytes" != "768" ]]; then
  echo "FAIL: unexpected RGB byte count: $bytes (want 768)" >&2
  exit 1
fi

sha="$(sha256sum "$out_rgb" | awk '{print $1}')"

if [[ $update -eq 1 ]]; then
  echo "$sha" > "$expected_file"
  echo "UPDATED: $expected_file" >&2
  exit 0
fi

if [[ ! -f "$expected_file" ]]; then
  echo "FAIL: missing $expected_file (run with --update once)" >&2
  exit 1
fi

expected="$(cat "$expected_file" | tr -d '\n\r ' )"
if [[ "$sha" != "$expected" ]]; then
  echo "FAIL: RGB sha256 mismatch" >&2
  echo "  got:  $sha" >&2
  echo "  want: $expected" >&2
  exit 1
fi

echo "OK: enc m03 miniframe decodes and RGB hash matches" >&2
