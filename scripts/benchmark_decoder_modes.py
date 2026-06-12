#!/usr/bin/env python3
"""Benchmark decoder modes over the local images/**/*.webp corpus."""

from __future__ import annotations

import csv
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


MODES = ("-info", "-yuv", "-yuvf", "-ppm", "-png")
WIDTH_RE = re.compile(r"^\s*Width:\s+(\d+)\s*$")
HEIGHT_RE = re.compile(r"^\s*Height:\s+(\d+)\s*$")


def run_quiet(cmd: list[str]) -> float:
    start = time.perf_counter_ns()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    end = time.perf_counter_ns()
    return (end - start) / 1_000_000_000.0


def dimensions(decoder: str, webp: Path) -> tuple[int, int]:
    out = subprocess.check_output([decoder, "-info", str(webp)], text=True, stderr=subprocess.DEVNULL)
    width = height = None
    for line in out.splitlines():
        wm = WIDTH_RE.match(line)
        if wm:
            width = int(wm.group(1))
            continue
        hm = HEIGHT_RE.match(line)
        if hm:
            height = int(hm.group(1))
    if width is None or height is None:
        raise RuntimeError(f"could not parse dimensions from {webp}")
    return width, height


def mode_command(decoder: str, mode: str, webp: Path, out_dir: Path, index: int) -> list[str]:
    if mode == "-info":
        return [decoder, mode, str(webp)]
    suffix = {
        "-yuv": "i420",
        "-yuvf": "i420",
        "-ppm": "ppm",
        "-png": "png",
    }[mode]
    return [decoder, mode, str(webp), str(out_dir / f"{index:05d}{mode}.{suffix}")]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)

    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/test-artifacts/benchmark_decoder_modes")
    out_dir.mkdir(parents=True, exist_ok=True)
    outputs = out_dir / "outputs"
    outputs.mkdir(parents=True, exist_ok=True)

    decoder = os.environ.get("DECODER", "./build/decoder")
    runs = int(os.environ.get("RUNS", "5"))
    webps = sorted(Path("images").glob("**/*.webp"))
    if not webps:
        print("error: no images/**/*.webp files found", file=sys.stderr)
        return 2
    if not Path(decoder).exists():
        print(f"error: decoder not found: {decoder}", file=sys.stderr)
        return 2

    dims: list[tuple[int, int]] = []
    for webp in webps:
        dims.append(dimensions(decoder, webp))
    total_mp = sum(w * h for w, h in dims) / 1_000_000.0

    times_path = out_dir / "decoder_modes_times.csv"
    summary_path = out_dir / "decoder_modes_summary.csv"

    rows: list[dict[str, str]] = []
    summary_rows: list[dict[str, str]] = []
    for mode in MODES:
        aggregate_times: list[float] = []
        for run in range(1, runs + 1):
            total = 0.0
            for idx, webp in enumerate(webps):
                total += run_quiet(mode_command(decoder, mode, webp, outputs, idx))
            aggregate_times.append(total)
            rows.append({"mode": mode, "run": str(run), "seconds": f"{total:.6f}"})

        median = statistics.median(aggregate_times)
        best = min(aggregate_times)
        worst = max(aggregate_times)
        summary_rows.append(
            {
                "mode": mode,
                "files": str(len(webps)),
                "megapixels": f"{total_mp:.3f}",
                "runs": str(runs),
                "median_seconds": f"{median:.6f}",
                "best_seconds": f"{best:.6f}",
                "worst_seconds": f"{worst:.6f}",
                "median_mp_s": f"{total_mp / median:.2f}",
            }
        )

    with times_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=("mode", "run", "seconds"))
        writer.writeheader()
        writer.writerows(rows)
    with summary_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=("mode", "files", "megapixels", "runs", "median_seconds", "best_seconds", "worst_seconds", "median_mp_s"),
        )
        writer.writeheader()
        writer.writerows(summary_rows)

    print(f"wrote {summary_path}")
    print(f"wrote {times_path}")
    for row in summary_rows:
        print(f"{row['mode']}: {row['median_seconds']}s / {row['median_mp_s']} MP/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
