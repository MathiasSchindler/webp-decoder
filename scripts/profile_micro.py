#!/usr/bin/env python3
"""Run opt-in VP8 micro-profiling and collect CSV artifacts."""

from __future__ import annotations

import argparse
import csv
import subprocess
import time
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def collect_inputs(root: Path) -> list[tuple[str, Path]]:
    items: list[tuple[str, Path]] = []
    commons = root / "images" / "commons" / "generated-webp"
    if commons.exists():
        for path in sorted(commons.glob("*.webp")):
            items.append(("commons-generated", path))
    for path in sorted((root / "images").rglob("*q80*.webp")):
        items.append(("q80-stress", path))

    seen: set[Path] = set()
    deduped: list[tuple[str, Path]] = []
    for source, path in items:
        key = path.resolve()
        if key in seen:
            continue
        seen.add(key)
        deduped.append((source, path))
    return deduped


def parse_micro_csv(text: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    reader = csv.DictReader(text.splitlines())
    for row in reader:
        if row.get("kind") and row.get("name") and row.get("value"):
            rows.append(row)
    return rows


def run_profile(decoder: Path, webp: Path) -> tuple[float, list[dict[str, str]]]:
    start = time.perf_counter()
    out = subprocess.check_output([str(decoder), "-profile_micro", str(webp), "/dev/null"], text=True)
    return time.perf_counter() - start, parse_micro_csv(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decoder", default=str(repo_root() / "build" / "decoder"))
    parser.add_argument("--out-dir", default=str(repo_root() / "build" / "profile" / "sugg1-microprofiling"))
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--max-files", type=int, default=0)
    args = parser.parse_args()

    root = repo_root()
    decoder = Path(args.decoder)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    inputs = collect_inputs(root)
    if args.max_files > 0:
        inputs = inputs[: args.max_files]
    if not inputs:
        raise SystemExit("no WebP inputs found")

    combined_path = out_dir / "microprofile-combined.csv"
    manifest_path = out_dir / "microprofile-manifest.csv"
    with combined_path.open("w", newline="") as combined, manifest_path.open("w", newline="") as manifest:
        writer = csv.DictWriter(combined, fieldnames=["source", "file", "run", "kind", "name", "value"])
        writer.writeheader()
        manifest_writer = csv.DictWriter(manifest, fieldnames=["source", "file", "run", "seconds", "rows"])
        manifest_writer.writeheader()
        for source, webp in inputs:
            rel = str(webp.relative_to(root)) if webp.is_relative_to(root) else str(webp)
            for run in range(args.runs):
                seconds, rows = run_profile(decoder, webp)
                for row in rows:
                    writer.writerow({"source": source, "file": rel, "run": run, **row})
                manifest_writer.writerow(
                    {"source": source, "file": rel, "run": run, "seconds": f"{seconds:.6f}", "rows": len(rows)}
                )

    print(combined_path)
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
