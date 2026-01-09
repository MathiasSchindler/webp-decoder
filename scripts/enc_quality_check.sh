#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

EXPECTED=scripts/enc_quality_expected.txt

if [ "${1:-}" = "--update" ]; then
	scripts/enc_quality_manifest.sh > "$EXPECTED"
	echo "Updated $EXPECTED" >&2
	exit 0
fi

if [ ! -f "$EXPECTED" ]; then
	echo "Missing $EXPECTED" >&2
	echo "Run: scripts/enc_quality_check.sh --update" >&2
	exit 2
fi

TMP=$(mk_artifact_tmpfile)
scripts/enc_quality_manifest.sh > "$TMP"

python3 - "$EXPECTED" "$TMP" <<'PY'
import math
import sys

expected_path = sys.argv[1]
current_path = sys.argv[2]

TOL_DB = 0.05  # allow small libm formatting drift; reject real regressions
TOL_SSIM = 0.0005

def parse_line(line: str):
    line = line.strip()
    if not line:
        return None
    parts = line.split()
    if len(parts) < 2:
        raise ValueError(f"bad line: {line!r}")
    path = parts[0]
    kv = {}
    for p in parts[1:]:
        if "=" not in p:
            continue
        k, v = p.split("=", 1)
        if v == "inf":
            kv[k] = math.inf
        else:
            kv[k] = float(v)
    return path, kv

def load(path: str):
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            parsed = parse_line(line)
            if parsed is None:
                continue
            p, kv = parsed
            out[p] = kv
    return out

expected = load(expected_path)
current = load(current_path)

missing = sorted(set(expected) - set(current))
extra = sorted(set(current) - set(expected))
if missing:
    print("FAIL: missing images in current manifest:")
    for p in missing:
        print("  ", p)
    sys.exit(1)
if extra:
    print("FAIL: unexpected extra images in current manifest:")
    for p in extra:
        print("  ", p)
    sys.exit(1)

keys = ["psnr_rgb", "psnr_r", "psnr_g", "psnr_b", "ssim_y"]

failures = []
for p in sorted(expected):
    e = expected[p]
    c = current[p]
    for k in keys:
        if k not in e:
            continue
        if k not in c:
            failures.append(f"{p}: missing {k} in current")
            continue
        ev = e[k]
        cv = c[k]
        if k == "ssim_y":
            if cv + TOL_SSIM < ev:
                failures.append(f"{p}: {k} regressed {ev:.6f} -> {cv:.6f} (tol {TOL_SSIM})")
            continue

        if math.isinf(ev) and not math.isinf(cv):
            failures.append(f"{p}: {k} regressed from inf to {cv}")
            continue
        if (not math.isinf(ev)) and (cv + TOL_DB < ev):
            failures.append(f"{p}: {k} regressed {ev:.6f} -> {cv:.6f} (tol {TOL_DB} dB)")

if failures:
    print("FAIL: PSNR regressions detected:")
    for f in failures[:50]:
        print("  ", f)
    if len(failures) > 50:
        print(f"  ... and {len(failures) - 50} more")
    sys.exit(1)

print(
    f"OK: encoder PSNR/SSIM guardrail holds for {len(expected)} images "
    f"(tol_psnr={TOL_DB} dB tol_ssim={TOL_SSIM})",
    file=sys.stderr,
)
PY
