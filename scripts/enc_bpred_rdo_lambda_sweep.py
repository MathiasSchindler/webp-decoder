#!/usr/bin/env python3
"""Sweep bpred-rdo lambda scaling without adding new encoder modes.

This script is intentionally narrow in scope:
- It only tunes the existing experimental `--mode bpred-rdo`.
- It does not introduce additional bpred variants.

It uses the local fast harness (ours encode + ours decode + metrics) and prints
ranked settings by mean SSIM, then mean bytes.

Example:
  ./scripts/enc_bpred_rdo_lambda_sweep.py images/commons-hq --sizes 256 --qs 40 60 80 \
    --mul 1 2 3 4 6 8 --div 1 2 3 4

Note: run `make all enc_png2ppm enc_quality_metrics` once upfront for speed.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


_OVERALL_RE = re.compile(
    r"Overall \(mean\): PSNR_RGB=([0-9.]+)\s+SSIM_Y=([0-9.]+)\s+bytes=([0-9.]+)"
)


def _collect_images(inputs: list[str]) -> list[str]:
    exts = {".jpg", ".jpeg", ".png"}
    out: list[str] = []
    for inp in inputs:
        p = Path(inp)
        if p.is_dir():
            for child in sorted(p.iterdir()):
                if child.is_file() and child.suffix.lower() in exts:
                    out.append(str(child))
        else:
            if p.is_file() and p.suffix.lower() in exts:
                out.append(str(p))
    return out


def _run_one(images: list[str], sizes: str, qs: str, mul: int, div: int, jobsafe: bool) -> tuple[int, int, float, float, float]:
    env = os.environ.copy()
    env["SIZES"] = sizes
    env["QS"] = qs
    env["MODE"] = "bpred-rdo"
    env["OURS_FLAGS"] = f"--loopfilter --bpred-rdo-lambda-mul {mul} --bpred-rdo-lambda-div {div}"
    if jobsafe:
        env["SKIP_BUILD"] = "1"

    p = subprocess.run(
        ["./scripts/enc_bpred_rdo_local_fast.sh", *images],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if p.returncode != 0:
        raise RuntimeError(f"mul={mul} div={div} failed:\n{p.stderr}")

    overall = None
    for ln in reversed(p.stdout.splitlines()):
        ln = ln.strip()
        if ln.startswith("Overall"):
            overall = ln
            break
    if not overall:
        raise RuntimeError(f"mul={mul} div={div}: missing Overall")

    m = _OVERALL_RE.search(overall)
    if not m:
        raise RuntimeError(f"mul={mul} div={div}: parse failed: {overall}")

    psnr = float(m.group(1))
    ssim = float(m.group(2))
    byt = float(m.group(3))
    return (mul, div, psnr, ssim, byt)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="Image files and/or directories")
    ap.add_argument("--sizes", nargs="+", default=["256"], help="Resize sizes (max dimension). Default: 256")
    ap.add_argument("--qs", nargs="+", default=["40", "60", "80"], help="Quality sweep. Default: 40 60 80")
    ap.add_argument("--mul", nargs="+", type=int, default=[1, 2, 3, 4, 6, 8], help="Lambda multipliers to try")
    ap.add_argument("--div", nargs="+", type=int, default=[1, 2, 3, 4], help="Lambda divisors to try")
    ap.add_argument("-j", "--jobs", type=int, default=4, help="Parallel jobs. Default: 4")
    ap.add_argument("--no-skip-build", action="store_true", help="Do not set SKIP_BUILD=1")
    args = ap.parse_args(argv)

    images = _collect_images(args.inputs)
    if not images:
        print("error: no input images found", file=sys.stderr)
        return 2

    sizes = " ".join(args.sizes)
    qs = " ".join(args.qs)

    # If we are running multiple jobs, it's safer to skip the per-run make step.
    jobsafe = not args.no_skip_build

    pairs = [(mul, div) for mul in args.mul for div in args.div]

    rows: list[tuple[int, int, float, float, float]] = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [ex.submit(_run_one, images, sizes, qs, mul, div, jobsafe) for (mul, div) in pairs]
        for fut in as_completed(futs):
            rows.append(fut.result())

    rows.sort(key=lambda r: (-r[3], r[4], -r[2]))

    print(f"Corpus: {len(images)} images")
    print(f"SIZES={sizes}  QS={qs}")
    print("Ranked by mean SSIM desc, mean bytes asc:")
    for mul, div, psnr, ssim, byt in rows[:15]:
        print(f"mul={mul:<2} div={div:<2}  PSNR={psnr:6.3f}  SSIM={ssim:.6f}  bytes={byt:9.1f}")

    best = rows[0]
    print("\nBest:")
    print(f"--bpred-rdo-lambda-mul {best[0]} --bpred-rdo-lambda-div {best[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
