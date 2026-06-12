#!/usr/bin/env python3
"""Reproducible stage-level and comparative decode profiling."""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


WIDTH_RE = re.compile(r"^\s*Width:\s+(\d+)\s*$")
HEIGHT_RE = re.compile(r"^\s*Height:\s+(\d+)\s*$")
QUALITY_RE = re.compile(r"(?:^|-q)(\d+)(?:\.webp)?$")

OURS_MODES = (
    ("ours-cumulative", "our-info", "container_header_info", "-info", "container/header/info (+ stats)"),
    ("ours-cumulative", "our-yuv", "yuv_no_loopfilter", "-yuv", "YUV decode/reconstruct without loopfilter"),
    ("ours-cumulative", "our-yuvf", "yuv_with_loopfilter", "-yuvf", "YUV decode/reconstruct with loopfilter"),
    ("ours-cumulative", "our-ppm", "ppm_rgb_output", "-ppm", "filtered YUV + RGB/PPM output"),
    ("ours-cumulative", "our-png", "png_output", "-png", "filtered YUV + PNG output"),
)

LIBWEBP_HELPER_SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>

static int read_file(const char* path, uint8_t** data, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return 0; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
  uint8_t* p = (uint8_t*)malloc((size_t)n);
  if (!p) { fclose(f); return 0; }
  if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return 0; }
  fclose(f);
  *data = p;
  *size = (size_t)n;
  return 1;
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    int v = WebPGetDecoderVersion();
    printf("%d.%d.%d\n", (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return 0;
  }
  if (argc != 3) {
    fprintf(stderr, "usage: %s in.webp out.ppm\n", argv[0]);
    return 2;
  }
  uint8_t* data = NULL;
  size_t size = 0;
  if (!read_file(argv[1], &data, &size)) return 1;
  int w = 0, h = 0;
  if (!WebPGetInfo(data, size, &w, &h) || w <= 0 || h <= 0) { free(data); return 1; }
  size_t rgb_size = (size_t)w * (size_t)h * 3u;
  uint8_t* rgb = (uint8_t*)malloc(rgb_size);
  if (!rgb) { free(data); return 1; }
  uint8_t* ok = WebPDecodeRGBInto(data, size, rgb, rgb_size, w * 3);
  free(data);
  if (!ok) { free(rgb); return 1; }
  FILE* out = fopen(argv[2], "wb");
  if (!out) { free(rgb); return 1; }
  fprintf(out, "P6\n%d %d\n255\n", w, h);
  int rc = fwrite(rgb, 1, rgb_size, out) == rgb_size ? 0 : 1;
  fclose(out);
  free(rgb);
  return rc;
}
""".lstrip()


@dataclass(frozen=True)
class CorpusItem:
    path: Path
    width: int
    height: int
    megapixels: float
    quality: str
    source: str
    bytes: int


@dataclass(frozen=True)
class Benchmark:
    group: str
    backend: str
    stage: str
    mode: str
    description: str
    command_template: str
    argv_template: tuple[str, ...]

    def command(self, webp: Path) -> list[str]:
        return [part.format(webp=str(webp), out="/dev/null") for part in self.argv_template]


def rel(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def run_text(cmd: list[str]) -> str:
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
        return out.strip().splitlines()[0] if out.strip() else ""
    except (OSError, subprocess.CalledProcessError):
        return ""


def run_quiet(cmd: list[str]) -> float:
    start = time.perf_counter_ns()
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    except OSError as exc:
        raise RuntimeError(f"failed to run {' '.join(cmd)}: {exc}") from exc
    end = time.perf_counter_ns()
    return (end - start) / 1_000_000_000.0


def parse_dimensions(decoder: str, webp: Path) -> tuple[int, int]:
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


def infer_quality(path: Path) -> str:
    for part in (path.stem, path.name):
        m = QUALITY_RE.search(part)
        if m:
            return m.group(1)
    m = re.search(r"-q(\d+)\.webp$", path.name)
    return m.group(1) if m else ""


def load_manifest(root: Path, manifest: Path) -> list[CorpusItem]:
    items: list[CorpusItem] = []
    with manifest.open(newline="") as f:
        for row in csv.DictReader(f):
            webp = root / row["webp"]
            if not webp.exists():
                continue
            width = int(row["width"])
            height = int(row["height"])
            mp = float(row.get("megapixels") or (width * height / 1_000_000.0))
            items.append(
                CorpusItem(
                    path=webp,
                    width=width,
                    height=height,
                    megapixels=mp,
                    quality=row.get("quality", ""),
                    source=row.get("source", ""),
                    bytes=int(row.get("bytes") or webp.stat().st_size),
                )
            )
    return items


def load_corpus(root: Path, decoder: str, args: argparse.Namespace) -> tuple[list[CorpusItem], str]:
    if args.webp_glob:
        paths = []
        for pattern in args.webp_glob:
            paths.extend(root.glob(pattern))
        source = "custom glob: " + ", ".join(args.webp_glob)
        manifest_items: list[CorpusItem] = []
    else:
        manifest = root / "build/profile/commons-decoder-benchmark/conversion_manifest.csv"
        manifest_items = load_manifest(root, manifest) if manifest.exists() else []
        if args.corpus in ("auto", "commons") and manifest_items:
            return sorted(manifest_items, key=lambda item: str(item.path)), rel(root, manifest)
        if args.corpus in ("auto", "commons"):
            paths = sorted((root / "images/commons/generated-webp").glob("*.webp"))
            source = "images/commons/generated-webp/*.webp"
        else:
            paths = sorted((root / "images").glob("**/*.webp"))
            source = "images/**/*.webp"

    seen: set[Path] = set()
    items: list[CorpusItem] = []
    for path in sorted(paths):
        if not path.is_file() or path in seen:
            continue
        seen.add(path)
        width, height = parse_dimensions(decoder, path)
        items.append(
            CorpusItem(
                path=path,
                width=width,
                height=height,
                megapixels=width * height / 1_000_000.0,
                quality=infer_quality(path),
                source="",
                bytes=path.stat().st_size,
            )
        )
    return items, source


def write_csv(path: Path, fieldnames: tuple[str, ...], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def command_template(argv: tuple[str, ...]) -> str:
    return " ".join(argv).replace("{webp}", "<in.webp>").replace("{out}", "/dev/null")


def build_libwebp_helper(root: Path, out_dir: Path, args: argparse.Namespace) -> tuple[Path | None, str, str]:
    if args.libwebp_helper:
        helper = Path(args.libwebp_helper)
        version = run_text([str(helper), "--version"]) or "unknown"
        return helper, version, "provided by --libwebp-helper"

    src = out_dir / "libwebp_decode_ppm.c"
    helper = out_dir / "libwebp_decode_ppm"
    src.write_text(LIBWEBP_HELPER_SOURCE)

    cc = os.environ.get("CC", "cc")
    compile_cmd = [cc, "-O3", "-Wall", "-Wextra", "-o", str(helper), str(src), "-lwebp"]
    try:
        subprocess.run(compile_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
        return helper, run_text([str(helper), "--version"]) or "unknown", "compiled: " + " ".join(compile_cmd)
    except (OSError, subprocess.CalledProcessError) as exc:
        fallback = root / "build/profile/commons-decoder-benchmark/libwebp_decode_ppm"
        if fallback.exists() and os.access(fallback, os.X_OK):
            return fallback, "unknown", f"fallback after compile failure: {exc}"
        return None, "", f"unavailable: {exc}"


def snapshot_decoder(out_dir: Path, decoder: str, no_snapshot: bool) -> tuple[str, str]:
    if no_snapshot:
        return decoder, "disabled"
    src = Path(decoder)
    dst = out_dir / "decoder-under-test"
    shutil.copy2(src, dst)
    dst.chmod(dst.stat().st_mode | 0o111)
    return str(dst), f"copied from {decoder}"


def detect_benchmarks(root: Path, out_dir: Path, args: argparse.Namespace) -> tuple[list[Benchmark], list[dict[str, object]], dict[str, str]]:
    benches = [
        Benchmark(
            group=group,
            backend=backend,
            stage=stage,
            mode=mode,
            description=description,
            command_template=command_template((args.decoder, mode, "{webp}") if mode == "-info" else (args.decoder, mode, "{webp}", "{out}")),
            argv_template=(args.decoder, mode, "{webp}") if mode == "-info" else (args.decoder, mode, "{webp}", "{out}"),
        )
        for group, backend, stage, mode, description in OURS_MODES
    ]

    status_rows: list[dict[str, object]] = []
    versions: dict[str, str] = {}

    if not args.skip_comparative:
        helper, version, status = build_libwebp_helper(root, out_dir, args)
        status_rows.append({"tool": "libwebp-api", "enabled": bool(helper), "version": version, "status": status})
        if helper:
            versions["libwebp-api"] = version
            argv = (str(helper), "{webp}", "{out}")
            benches.append(
                Benchmark("comparative-whole-ppm", "libwebp-api", "whole_ppm", "api-rgb-ppm", "system libwebp API RGB+PPM", command_template(argv), argv)
            )

        ffmpeg = shutil.which("ffmpeg")
        ffmpeg_version = run_text([ffmpeg, "-version"]) if ffmpeg else ""
        status_rows.append({"tool": "ffmpeg", "enabled": bool(ffmpeg), "version": ffmpeg_version, "status": ffmpeg or "not found"})
        if ffmpeg:
            versions["ffmpeg"] = ffmpeg_version
            argv = (ffmpeg, "-nostdin", "-v", "error", "-y", "-i", "{webp}", "-f", "image2pipe", "-vcodec", "ppm", "{out}")
            benches.append(Benchmark("comparative-whole-ppm", "ffmpeg", "whole_ppm", "ppm", "ffmpeg WebP decode to PPM", command_template(argv), argv))

        magick = shutil.which("magick") or shutil.which("convert")
        magick_version = run_text([magick, "-version"]) if magick else ""
        status_rows.append({"tool": "imagemagick", "enabled": bool(magick), "version": magick_version, "status": magick or "not found"})
        if magick:
            versions["imagemagick"] = magick_version
            argv = (magick, "{webp}", "PPM:{out}")
            benches.append(Benchmark("comparative-whole-ppm", "imagemagick", "whole_ppm", "ppm", "ImageMagick WebP decode to PPM", command_template(argv), argv))

    versions["ours"] = run_text(["file", args.decoder]) if shutil.which("file") else args.decoder
    return benches, status_rows, versions


def benchmark(benches: list[Benchmark], items: list[CorpusItem], runs: int, warmups: int, root: Path) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    file_rows: list[dict[str, object]] = []
    run_rows: list[dict[str, object]] = []
    summary_rows: list[dict[str, object]] = []
    total_mp = sum(item.megapixels for item in items)

    for bench in benches:
        for _ in range(warmups):
            for item in items:
                run_quiet(bench.command(item.path))

        aggregate_times: list[float] = []
        for run in range(1, runs + 1):
            total = 0.0
            for item in items:
                seconds = run_quiet(bench.command(item.path))
                total += seconds
                file_rows.append(
                    {
                        "group": bench.group,
                        "backend": bench.backend,
                        "stage": bench.stage,
                        "mode": bench.mode,
                        "run": run,
                        "file": rel(root, item.path),
                        "width": item.width,
                        "height": item.height,
                        "megapixels": f"{item.megapixels:.6f}",
                        "quality": item.quality,
                        "seconds": f"{seconds:.9f}",
                    }
                )
            aggregate_times.append(total)
            run_rows.append(
                {
                    "group": bench.group,
                    "backend": bench.backend,
                    "stage": bench.stage,
                    "mode": bench.mode,
                    "run": run,
                    "files": len(items),
                    "megapixels": f"{total_mp:.6f}",
                    "seconds": f"{total:.9f}",
                    "mp_s": f"{total_mp / total:.2f}" if total > 0 else "",
                }
            )

        median = statistics.median(aggregate_times)
        best = min(aggregate_times)
        worst = max(aggregate_times)
        summary_rows.append(
            {
                "group": bench.group,
                "backend": bench.backend,
                "stage": bench.stage,
                "mode": bench.mode,
                "description": bench.description,
                "files": len(items),
                "megapixels": f"{total_mp:.6f}",
                "runs": runs,
                "median_seconds": f"{median:.9f}",
                "best_seconds": f"{best:.9f}",
                "worst_seconds": f"{worst:.9f}",
                "median_mp_s": f"{total_mp / median:.2f}" if median > 0 else "",
                "command_template": bench.command_template,
            }
        )
    return file_rows, run_rows, summary_rows


def build_increment_rows(summary_rows: list[dict[str, object]], total_mp: float, runs: int) -> list[dict[str, object]]:
    by_stage = {str(row["stage"]): float(row["median_seconds"]) for row in summary_rows if row["group"] == "ours-cumulative"}
    specs = (
        ("container_header_info", "container_header_info", "", "our -info cumulative"),
        ("decode_to_yuv_no_loopfilter", "yuv_no_loopfilter", "", "our -yuv cumulative"),
        ("loopfilter_increment", "yuv_with_loopfilter", "yuv_no_loopfilter", "our -yuvf minus -yuv"),
        ("ppm_rgb_output_increment", "ppm_rgb_output", "yuv_with_loopfilter", "our -ppm minus -yuvf"),
        ("png_output_increment", "png_output", "yuv_with_loopfilter", "our -png minus -yuvf"),
    )
    rows: list[dict[str, object]] = []
    for stage, minuend, subtrahend, description in specs:
        if minuend not in by_stage:
            continue
        seconds = by_stage[minuend] - by_stage.get(subtrahend, 0.0)
        rows.append(
            {
                "group": "ours-derived",
                "stage": stage,
                "description": description,
                "runs": runs,
                "megapixels": f"{total_mp:.6f}",
                "median_seconds": f"{seconds:.9f}",
                "median_mp_s": f"{total_mp / seconds:.2f}" if seconds > 0 else "",
                "derived_from": minuend,
                "minus": subtrahend,
            }
        )
    return rows


def write_readme(out_dir: Path, args: argparse.Namespace, corpus_source: str, total_mp: float, summary_rows: list[dict[str, object]], increment_rows: list[dict[str, object]], versions: dict[str, str]) -> None:
    lines = [
        "# Decode stage profile",
        "",
        f"Date: {datetime.now(timezone.utc).isoformat()}",
        "",
        "## Corpus",
        "",
        f"- Source: `{corpus_source}`",
        f"- Files: {summary_rows[0]['files'] if summary_rows else 0}",
        f"- Megapixels per run: {total_mp:.6f}",
        f"- Runs: {args.runs}",
        f"- Warmups: {args.warmups}",
        "- Output target: `/dev/null`",
        "",
        "## Summary",
        "",
        "| Group | Backend | Stage | Median seconds | MP/s | Command |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    for row in summary_rows:
        lines.append(
            f"| {row['group']} | {row['backend']} | {row['stage']} | {row['median_seconds']} | {row['median_mp_s']} | `{row['command_template']}` |"
        )
    lines.extend(["", "## Derived stage deltas", "", "| Stage | Median seconds | MP/s | Basis |", "| --- | ---: | ---: | --- |"])
    for row in increment_rows:
        basis = str(row["description"])
        lines.append(f"| {row['stage']} | {row['median_seconds']} | {row['median_mp_s']} | {basis} |")
    lines.extend(["", "## Versions", ""])
    for key, value in sorted(versions.items()):
        lines.append(f"- {key}: `{value}`")
    lines.extend(
        [
            "",
            "## CSV artifacts",
            "",
            "- `corpus.csv`",
            "- `commands.csv`",
            "- `tool_status.csv`",
            "- `stage_profile_summary.csv`",
            "- `stage_profile_increments.csv`",
            "- `stage_profile_run_times.csv`",
            "- `stage_profile_file_times.csv`",
            "- `versions.json`",
        ]
    )
    (out_dir / "README.md").write_text("\n".join(lines) + "\n")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default="build/profile/commons-decoder-stage-profile", help="artifact directory")
    parser.add_argument("--decoder", default=os.environ.get("DECODER", "./build/decoder"), help="decoder binary")
    parser.add_argument("--runs", type=int, default=int(os.environ.get("RUNS", "5")), help="whole-corpus timed runs per command")
    parser.add_argument("--warmups", type=int, default=int(os.environ.get("WARMUPS", "0")), help="untimed warmup runs per command")
    parser.add_argument("--corpus", choices=("auto", "commons", "images"), default="auto", help="corpus selector")
    parser.add_argument("--webp-glob", action="append", help="custom WebP glob relative to repo root; may be repeated")
    parser.add_argument("--skip-comparative", action="store_true", help="only benchmark our decoder stages")
    parser.add_argument("--libwebp-helper", help="prebuilt libwebp API helper")
    parser.add_argument("--no-snapshot-decoder", action="store_true", help="run --decoder in place instead of copying it into the artifact directory")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.runs <= 0:
        print("error: --runs must be positive", file=sys.stderr)
        return 2
    if not Path(args.decoder).exists():
        print(f"error: decoder not found: {args.decoder}", file=sys.stderr)
        return 2

    items, corpus_source = load_corpus(root, args.decoder, args)
    if not items:
        print("error: no WebP files found for selected corpus", file=sys.stderr)
        return 2

    original_decoder = args.decoder
    args.decoder, decoder_snapshot = snapshot_decoder(out_dir, args.decoder, args.no_snapshot_decoder)
    benches, status_rows, versions = detect_benchmarks(root, out_dir, args)
    total_mp = sum(item.megapixels for item in items)
    versions.update(
        {
            "python": platform.python_version(),
            "system": platform.platform(),
            "host": socket.gethostname(),
            "git": run_text(["git", "rev-parse", "HEAD"]),
            "git_dirty": "yes" if run_text(["git", "status", "--short"]) else "no",
            "original_decoder": original_decoder,
            "decoder_snapshot": decoder_snapshot,
            "runs": str(args.runs),
            "files": str(len(items)),
            "total_mp": f"{total_mp:.6f}",
        }
    )

    command_rows = [
        {
            "group": bench.group,
            "backend": bench.backend,
            "stage": bench.stage,
            "mode": bench.mode,
            "description": bench.description,
            "command_template": bench.command_template,
        }
        for bench in benches
    ]
    corpus_rows = [
        {
            "file": rel(root, item.path),
            "width": item.width,
            "height": item.height,
            "megapixels": f"{item.megapixels:.6f}",
            "quality": item.quality,
            "bytes": item.bytes,
            "source": item.source,
        }
        for item in items
    ]

    file_rows, run_rows, summary_rows = benchmark(benches, items, args.runs, args.warmups, root)
    increment_rows = build_increment_rows(summary_rows, total_mp, args.runs)

    write_csv(out_dir / "corpus.csv", ("file", "width", "height", "megapixels", "quality", "bytes", "source"), corpus_rows)
    write_csv(out_dir / "commands.csv", ("group", "backend", "stage", "mode", "description", "command_template"), command_rows)
    write_csv(out_dir / "tool_status.csv", ("tool", "enabled", "version", "status"), status_rows)
    write_csv(
        out_dir / "stage_profile_file_times.csv",
        ("group", "backend", "stage", "mode", "run", "file", "width", "height", "megapixels", "quality", "seconds"),
        file_rows,
    )
    write_csv(
        out_dir / "stage_profile_run_times.csv",
        ("group", "backend", "stage", "mode", "run", "files", "megapixels", "seconds", "mp_s"),
        run_rows,
    )
    write_csv(
        out_dir / "stage_profile_summary.csv",
        (
            "group",
            "backend",
            "stage",
            "mode",
            "description",
            "files",
            "megapixels",
            "runs",
            "median_seconds",
            "best_seconds",
            "worst_seconds",
            "median_mp_s",
            "command_template",
        ),
        summary_rows,
    )
    write_csv(
        out_dir / "stage_profile_increments.csv",
        ("group", "stage", "description", "runs", "megapixels", "median_seconds", "median_mp_s", "derived_from", "minus"),
        increment_rows,
    )
    (out_dir / "versions.json").write_text(json.dumps(versions, indent=2, sort_keys=True) + "\n")
    write_readme(out_dir, args, corpus_source, total_mp, summary_rows, increment_rows, versions)

    print(f"wrote {out_dir / 'stage_profile_summary.csv'}")
    print(f"wrote {out_dir / 'stage_profile_increments.csv'}")
    for row in summary_rows:
        print(f"{row['backend']} {row['stage']}: {row['median_seconds']}s / {row['median_mp_s']} MP/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
