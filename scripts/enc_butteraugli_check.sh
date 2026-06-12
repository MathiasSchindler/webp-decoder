#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

EXPECTED=scripts/enc_butteraugli_expected.txt

if [ "${1:-}" = "--update" ]; then
	scripts/enc_butteraugli_manifest.sh > "$EXPECTED"
	echo "Updated $EXPECTED" >&2
	exit 0
fi

if [ ! -f "$EXPECTED" ]; then
	echo "Missing $EXPECTED" >&2
	echo "Run: scripts/enc_butteraugli_check.sh --update" >&2
	exit 2
fi

TMP=$(mk_artifact_tmpfile)
scripts/enc_butteraugli_manifest.sh > "$TMP"

python3 - "$EXPECTED" "$TMP" <<'PY'
import sys

expected_path = sys.argv[1]
current_path = sys.argv[2]

TOL_BUTTERAUGLI = 0.02      # lower is better
TOL_BYTES_REL = 0.03        # allow tiny bitrate drift

def parse_line(line: str):
    line = line.strip()
    if not line:
        return None
    parts = line.split()
    if len(parts) < 3:
        raise ValueError(f"bad line: {line!r}")
    path = parts[0]
    kv = {}
    for p in parts[1:]:
        if "=" not in p:
            continue
        k, v = p.split("=", 1)
        if k == "bytes":
            kv[k] = int(v)
        elif k == "butteraugli":
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

failures = []
for p in sorted(expected):
    e = expected[p]
    c = current[p]

    if "butteraugli" not in e or "butteraugli" not in c:
        failures.append(f"{p}: missing butteraugli metric")
    else:
        if c["butteraugli"] > e["butteraugli"] + TOL_BUTTERAUGLI:
            failures.append(
                f"{p}: butteraugli regressed {e['butteraugli']:.6f} -> {c['butteraugli']:.6f} "
                f"(tol {TOL_BUTTERAUGLI})"
            )

    if "bytes" not in e or "bytes" not in c:
        failures.append(f"{p}: missing bytes metric")
    else:
        max_allowed = int(e["bytes"] * (1.0 + TOL_BYTES_REL) + 0.5)
        if c["bytes"] > max_allowed:
            failures.append(
                f"{p}: bytes regressed {e['bytes']} -> {c['bytes']} "
                f"(tol {TOL_BYTES_REL * 100:.1f}%)"
            )

if failures:
    print("FAIL: Butteraugli/size regressions detected:")
    for f in failures[:50]:
        print("  ", f)
    if len(failures) > 50:
        print(f"  ... and {len(failures) - 50} more")
    sys.exit(1)

print(
    f"OK: encoder Butteraugli/size guardrail holds for {len(expected)} images "
    f"(tol_butteraugli={TOL_BUTTERAUGLI} tol_bytes={TOL_BYTES_REL * 100:.1f}%)",
    file=sys.stderr,
)
PY
