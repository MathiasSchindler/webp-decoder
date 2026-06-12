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
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


_OVERALL_RE = re.compile(
    r"Overall \(mean\): PSNR_RGB=([0-9.]+)\s+SSIM_Y=([0-9.]+)\s+bytes=([0-9.]+)"
)
_FLOAT_RE = re.compile(r"[0-9]+(?:\.[0-9]+)?")


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


def _run_one(
    images: list[str],
    sizes: str,
    qs: str,
    mul: int,
    div: int,
    jobsafe: bool,
    rate: str,
) -> tuple[int, int, float, float, float]:
    env = os.environ.copy()
    env["SIZES"] = sizes
    env["QS"] = qs
    env["MODE"] = "bpred-rdo"
    env["OURS_FLAGS"] = f"--loopfilter --bpred-rdo-lambda-mul {mul} --bpred-rdo-lambda-div {div}"
    if jobsafe:
        env["SKIP_BUILD"] = "1"

    # Optional: switch the rate estimator used by bpred-rdo.
    if rate != "proxy":
        env["OURS_FLAGS"] += f" --bpred-rdo-rate {rate}"

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


def _first_float(text: str) -> float | None:
    m = _FLOAT_RE.search(text)
    if not m:
        return None
    try:
        return float(m.group(0))
    except ValueError:
        return None


def _prepare_ref_png(src: str, size: int, out_png: str) -> None:
    magick = shutil.which("magick")
    if magick:
        subprocess.run(
            [magick, src, "-auto-orient", "-resize", f"{size}x{size}>", "-strip", f"PNG24:{out_png}"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return

    sips = shutil.which("sips")
    if sips:
        subprocess.run(
            [sips, "-Z", str(size), "-s", "format", "png", src, "--out", out_png],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return

    raise RuntimeError("Neither ImageMagick 'magick' nor macOS 'sips' found")


def _run_one_butteraugli(
    images: list[str],
    sizes_list: list[int],
    qs_list: list[int],
    mul: int,
    div: int,
    rate: str,
    root_dir: str,
) -> tuple[int, int, float, float]:
    encoder = os.path.join(root_dir, "encoder")
    decoder = os.path.join(root_dir, "decoder")
    butter = os.path.join(root_dir, "butteraugli_nolibc_png")

    if not os.path.exists(encoder) or not os.path.exists(decoder):
        raise RuntimeError("missing encoder/decoder binaries; run make all")
    if not os.path.exists(butter):
        raise RuntimeError("missing butteraugli_nolibc_png")

    ours_flags = [
        "--loopfilter",
        "--bpred-rdo-lambda-mul",
        str(mul),
        "--bpred-rdo-lambda-div",
        str(div),
    ]
    if rate != "proxy":
        ours_flags += ["--bpred-rdo-rate", rate]

    scores: list[float] = []
    sizes_bytes: list[int] = []

    with tempfile.TemporaryDirectory(prefix="enc_bpred_rdo_ba_") as tmp:
        for src in images:
            stem = Path(src).stem
            for size in sizes_list:
                ref_png = os.path.join(tmp, f"{stem}_{size}.ref.png")
                _prepare_ref_png(src, size, ref_png)

                for q in qs_list:
                    out_webp = os.path.join(tmp, f"{stem}_{size}_q{q}.webp")
                    out_png = os.path.join(tmp, f"{stem}_{size}_q{q}.png")

                    subprocess.run(
                        [encoder, "--q", str(q), "--mode", "bpred-rdo", *ours_flags, ref_png, out_webp],
                        check=True,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    subprocess.run(
                        [decoder, "-png", out_webp, out_png],
                        check=True,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )

                    sizes_bytes.append(os.path.getsize(out_webp))
                    p = subprocess.run(
                        [butter, ref_png, out_png],
                        check=False,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    score = _first_float(p.stdout) if p.stdout else None
                    if score is None and p.stderr:
                        score = _first_float(p.stderr)
                    if score is None:
                        raise RuntimeError(f"mul={mul} div={div}: failed to parse Butteraugli score")
                    scores.append(score)

    if not scores or not sizes_bytes:
        raise RuntimeError(f"mul={mul} div={div}: no scores collected")

    mean_ba = sum(scores) / float(len(scores))
    mean_bytes = sum(sizes_bytes) / float(len(sizes_bytes))
    return (mul, div, mean_ba, mean_bytes)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="Image files and/or directories")
    ap.add_argument("--sizes", nargs="+", default=["256"], help="Resize sizes (max dimension). Default: 256")
    ap.add_argument("--qs", nargs="+", default=["40", "60", "80"], help="Quality sweep. Default: 40 60 80")
    ap.add_argument("--mul", nargs="+", type=int, default=[1, 2, 3, 4, 6, 8], help="Lambda multipliers to try")
    ap.add_argument("--div", nargs="+", type=int, default=[1, 2, 3, 4], help="Lambda divisors to try")
    ap.add_argument(
        "--objective",
        choices=["ssim", "butteraugli"],
        default="ssim",
        help="Ranking objective. 'ssim' uses enc_bpred_rdo_local_fast; 'butteraugli' uses decoder+butteraugli_nolibc_png.",
    )
    ap.add_argument("--rate", choices=["proxy", "entropy"], default="proxy", help="bpred-rdo rate estimator")
    ap.add_argument("-j", "--jobs", type=int, default=4, help="Parallel jobs. Default: 4")
    ap.add_argument("--no-skip-build", action="store_true", help="Do not set SKIP_BUILD=1")
    args = ap.parse_args(argv)

    images = _collect_images(args.inputs)
    if not images:
        print("error: no input images found", file=sys.stderr)
        return 2

    sizes = " ".join(args.sizes)
    qs = " ".join(args.qs)

    try:
        sizes_list = [int(s) for s in args.sizes]
        qs_list = [int(q) for q in args.qs]
    except ValueError:
        print("error: --sizes and --qs must be integer lists", file=sys.stderr)
        return 2

    if any(s <= 0 for s in sizes_list):
        print("error: --sizes must be > 0", file=sys.stderr)
        return 2
    if any(q < 0 or q > 100 for q in qs_list):
        print("error: --qs values must be in [0, 100]", file=sys.stderr)
        return 2

    # If we are running multiple jobs, it's safer to skip the per-run make step.
    jobsafe = not args.no_skip_build

    pairs = [(mul, div) for mul in args.mul for div in args.div]

    print(f"Corpus: {len(images)} images")
    print(f"SIZES={sizes}  QS={qs}  objective={args.objective}")

    if args.objective == "butteraugli":
        root_dir = str(Path(__file__).resolve().parents[1])
        subprocess.run(["make", "-s", "all"], cwd=root_dir, check=True)

        rows_ba: list[tuple[int, int, float, float]] = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
            futs = [
                ex.submit(
                    _run_one_butteraugli,
                    images,
                    sizes_list,
                    qs_list,
                    mul,
                    div,
                    args.rate,
                    root_dir,
                )
                for (mul, div) in pairs
            ]
            for fut in as_completed(futs):
                rows_ba.append(fut.result())

        rows_ba.sort(key=lambda r: (r[2], r[3]))

        print("Ranked by mean Butteraugli asc, mean bytes asc:")
        for mul, div, ba, byt in rows_ba[:15]:
            print(f"mul={mul:<2} div={div:<2}  Butteraugli={ba:8.5f}  bytes={byt:9.1f}")

        best = rows_ba[0]
        print("\nBest:")
        print(f"--bpred-rdo-lambda-mul {best[0]} --bpred-rdo-lambda-div {best[1]}")
    else:
        root_dir = str(Path(__file__).resolve().parents[1])
        subprocess.run(["make", "-s", "all", "enc_png2ppm", "enc_quality_metrics"], cwd=root_dir, check=True)

        rows: list[tuple[int, int, float, float, float]] = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
            futs = [
                ex.submit(_run_one, images, sizes, qs, mul, div, jobsafe, args.rate)
                for (mul, div) in pairs
            ]
            for fut in as_completed(futs):
                rows.append(fut.result())

        rows.sort(key=lambda r: (-r[3], r[4], -r[2]))

        print("Ranked by mean SSIM desc, mean bytes asc:")
        for mul, div, psnr, ssim, byt in rows[:15]:
            print(f"mul={mul:<2} div={div:<2}  PSNR={psnr:6.3f}  SSIM={ssim:.6f}  bytes={byt:9.1f}")

        best = rows[0]
        print("\nBest:")
        print(f"--bpred-rdo-lambda-mul {best[0]} --bpred-rdo-lambda-div {best[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
