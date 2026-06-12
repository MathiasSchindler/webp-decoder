#!/usr/bin/env python3
"""Benchmark our decoder vs libwebp over a quality x pixel-size grid."""

from __future__ import annotations

import argparse
import csv
import html
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


LIBWEBP_HELPER_SOURCE = r"""
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>

static volatile uint8_t g_sink;

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

static int checked_mul3_size(int w, int h, size_t* size) {
  if (w <= 0 || h <= 0) return 0;
  size_t pixels = (size_t)w * (size_t)h;
  if (pixels > SIZE_MAX / 3u) return 0;
  *size = pixels * 3u;
  return 1;
}

static int decode_rgb_ppm(const uint8_t* data, size_t size, const char* out_path) {
  int w = 0, h = 0;
  if (!WebPGetInfo(data, size, &w, &h)) return 1;
  size_t rgb_size = 0;
  if (!checked_mul3_size(w, h, &rgb_size)) return 1;
  uint8_t* rgb = (uint8_t*)malloc(rgb_size);
  if (!rgb) return 1;
  uint8_t* ok = WebPDecodeRGBInto(data, size, rgb, rgb_size, w * 3);
  if (!ok) { free(rgb); return 1; }
  g_sink ^= rgb[0];
  g_sink ^= rgb[rgb_size / 2];
  g_sink ^= rgb[rgb_size - 1];
  FILE* out = fopen(out_path, "wb");
  if (!out) { free(rgb); return 1; }
  int rc = 0;
  if (fprintf(out, "P6\n%d %d\n255\n", w, h) < 0) rc = 1;
  if (!rc && fwrite(rgb, 1, rgb_size, out) != rgb_size) rc = 1;
  if (fclose(out) != 0) rc = 1;
  free(rgb);
  return rc;
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    int v = WebPGetDecoderVersion();
    printf("%d.%d.%d\n", (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return 0;
  }
  if (argc != 4 || strcmp(argv[1], "ppm") != 0) {
    fprintf(stderr, "usage: %s ppm in.webp out.ppm\n", argv[0]);
    return 2;
  }
  uint8_t* data = NULL;
  size_t size = 0;
  if (!read_file(argv[2], &data, &size)) return 1;
  int rc = decode_rgb_ppm(data, size, argv[3]);
  free(data);
  return rc;
}
""".lstrip()


@dataclass(frozen=True)
class SourceImage:
    path: Path
    width: int
    height: int

    @property
    def pixels(self) -> int:
        return self.width * self.height


@dataclass(frozen=True)
class Variant:
    source: SourceImage
    target_mpix: float
    quality: str
    width: int
    height: int
    webp: Path

    @property
    def pixels(self) -> int:
        return self.width * self.height

    @property
    def megapixels(self) -> float:
        return self.pixels / 1_000_000.0


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run(argv: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(argv, cwd=cwd, check=True)


def run_text(argv: list[str]) -> str:
    return subprocess.check_output(argv, text=True).strip()


def parse_csv_floats(value: str) -> list[float]:
    out: list[float] = []
    for part in value.split(","):
        part = part.strip()
        if part:
            out.append(float(part))
    return out


def quality_label(q: float) -> str:
    if abs(q - round(q)) < 1e-9:
        return str(int(round(q)))
    return ("%g" % q).replace(".", "p")


def quality_magick_arg(q: str) -> str:
    return q.replace("p", ".")


def target_dimensions(src: SourceImage, target_mpix: float) -> tuple[int, int] | None:
    target_pixels = target_mpix * 1_000_000.0
    if target_pixels > src.pixels * 1.01:
        return None
    scale = min(1.0, math.sqrt(target_pixels / src.pixels))
    width = max(1, int(round(src.width * scale)))
    height = max(1, int(round(src.height * scale)))
    return width, height


def identify_sources(patterns: list[str]) -> list[SourceImage]:
    root = repo_root()
    paths: list[Path] = []
    for pattern in patterns:
        paths.extend(sorted(root.glob(pattern)))
    seen: set[Path] = set()
    sources: list[SourceImage] = []
    for path in paths:
        if path in seen:
            continue
        seen.add(path)
        text = run_text(["magick", "identify", "-format", "%w %h", str(path)])
        width_s, height_s = text.split()
        sources.append(SourceImage(path=path, width=int(width_s), height=int(height_s)))
    return sources


def build_libwebp_helper(out_dir: Path) -> Path:
    helper = out_dir / "libwebp_decode_ppm"
    src = out_dir / "libwebp_decode_ppm.c"
    src.write_text(LIBWEBP_HELPER_SOURCE)
    cc = os.environ.get("CC", "cc")
    run([cc, "-std=c11", "-O3", "-Wall", "-Wextra", "-o", str(helper), str(src), "-lwebp"])
    return helper


def generate_variant(variant: Variant, force: bool) -> None:
    if variant.webp.exists() and not force:
        return
    variant.webp.parent.mkdir(parents=True, exist_ok=True)
    run([
        "magick",
        str(variant.source.path),
        "-resize",
        f"{variant.width}x{variant.height}!",
        "-strip",
        "-quality",
        quality_magick_arg(variant.quality),
        "-define",
        "webp:lossless=false",
        str(variant.webp),
    ])


def time_command(argv: list[str], warmups: int, runs_count: int) -> tuple[float, float, float]:
    for _ in range(warmups):
        run(argv)
    timings: list[float] = []
    for _ in range(runs_count):
        start = time.perf_counter_ns()
        run(argv)
        end = time.perf_counter_ns()
        timings.append((end - start) / 1_000_000_000.0)
    return statistics.median(timings), min(timings), max(timings)


def color_for_advantage(percent: float) -> str:
    intensity = min(abs(percent) / 60.0, 1.0)
    if percent >= 0.0:
        r = int(245 * (1.0 - intensity) + 30 * intensity)
        g = int(250 * (1.0 - intensity) + 150 * intensity)
        b = int(245 * (1.0 - intensity) + 60 * intensity)
    else:
        r = int(250 * (1.0 - intensity) + 190 * intensity)
        g = int(245 * (1.0 - intensity) + 45 * intensity)
        b = int(245 * (1.0 - intensity) + 45 * intensity)
    return f"rgb({r},{g},{b})"


def color_for_delta(delta_pp: float) -> str:
    intensity = min(abs(delta_pp) / 25.0, 1.0)
    if delta_pp >= 0.0:
        r = int(245 * (1.0 - intensity) + 30 * intensity)
        g = int(250 * (1.0 - intensity) + 150 * intensity)
        b = int(245 * (1.0 - intensity) + 60 * intensity)
    else:
        r = int(250 * (1.0 - intensity) + 190 * intensity)
        g = int(245 * (1.0 - intensity) + 45 * intensity)
        b = int(245 * (1.0 - intensity) + 45 * intensity)
    return f"rgb({r},{g},{b})"


def row_float(row: dict[str, object], name: str) -> float:
    return float(row[name])


def matrix_key(row: dict[str, object]) -> tuple[str, float]:
    return str(row["quality"]), round(float(row["target_mpix"]), 9)


def faster_label(advantage_pct: float) -> str:
    if advantage_pct > 0.0005:
        return "ours"
    if advantage_pct < -0.0005:
        return "libwebp"
    return "tie"


def relative_delta_pct(current: float, baseline: float) -> str:
    if baseline == 0.0:
        return ""
    return f"{(current / baseline - 1.0) * 100.0:.3f}"


def write_dict_csv(path: Path, rows: list[dict[str, object]], fieldnames: list[str]) -> None:
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def load_matrix_csv(path: Path) -> dict[tuple[str, float], dict[str, object]]:
    rows: dict[tuple[str, float], dict[str, object]] = {}
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        required = {"target_mpix", "quality", "ours_mp_s", "libwebp_mp_s", "ours_advantage_pct"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise ValueError(f"{path} is missing required columns: {', '.join(sorted(missing))}")
        for row in reader:
            rows[matrix_key(row)] = row
    return rows


def compute_regression_rows(
    cell_rows: list[dict[str, object]],
    baseline_rows: dict[tuple[str, float], dict[str, object]],
) -> list[dict[str, object]]:
    delta_rows: list[dict[str, object]] = []
    for current in cell_rows:
        baseline = baseline_rows.get(matrix_key(current))
        if baseline is None:
            continue
        current_adv = row_float(current, "ours_advantage_pct")
        baseline_adv = row_float(baseline, "ours_advantage_pct")
        current_ours = row_float(current, "ours_mp_s")
        baseline_ours = row_float(baseline, "ours_mp_s")
        current_lib = row_float(current, "libwebp_mp_s")
        baseline_lib = row_float(baseline, "libwebp_mp_s")
        delta_rows.append({
            "target_mpix": current["target_mpix"],
            "quality": current["quality"],
            "current_files": current["files"],
            "baseline_files": baseline.get("files", ""),
            "current_ours_advantage_pct": f"{current_adv:.3f}",
            "baseline_ours_advantage_pct": f"{baseline_adv:.3f}",
            "delta_advantage_pp": f"{current_adv - baseline_adv:.3f}",
            "current_ours_mp_s": f"{current_ours:.3f}",
            "baseline_ours_mp_s": f"{baseline_ours:.3f}",
            "delta_ours_mp_s": f"{current_ours - baseline_ours:.3f}",
            "delta_ours_mp_s_pct": relative_delta_pct(current_ours, baseline_ours),
            "current_libwebp_mp_s": f"{current_lib:.3f}",
            "baseline_libwebp_mp_s": f"{baseline_lib:.3f}",
            "delta_libwebp_mp_s": f"{current_lib - baseline_lib:.3f}",
            "delta_libwebp_mp_s_pct": relative_delta_pct(current_lib, baseline_lib),
            "current_faster": faster_label(current_adv),
            "baseline_faster": faster_label(baseline_adv),
        })
    return delta_rows


def aggregate_cells(cell_rows: list[dict[str, object]], group_field: str) -> list[dict[str, object]]:
    groups: dict[str, list[dict[str, object]]] = {}
    for row in cell_rows:
        value = str(row[group_field])
        groups.setdefault(value, []).append(row)

    out: list[dict[str, object]] = []
    for value, rows in groups.items():
        mp = sum(row_float(r, "megapixels") for r in rows)
        ours_s = sum(row_float(r, "ours_seconds") for r in rows)
        lib_s = sum(row_float(r, "libwebp_seconds") for r in rows)
        advantages = [row_float(r, "ours_advantage_pct") for r in rows]
        labels = [faster_label(a) for a in advantages]
        out.append({
            "dimension": group_field,
            "value": value,
            "cells": len(rows),
            "ours_faster_cells": labels.count("ours"),
            "libwebp_faster_cells": labels.count("libwebp"),
            "tied_cells": labels.count("tie"),
            "megapixels": f"{mp:.6f}",
            "ours_seconds": f"{ours_s:.9f}",
            "libwebp_seconds": f"{lib_s:.9f}",
            "ours_mp_s": f"{(mp / ours_s) if ours_s else 0.0:.3f}",
            "libwebp_mp_s": f"{(mp / lib_s) if lib_s else 0.0:.3f}",
            "ours_advantage_pct": f"{((lib_s / ours_s - 1.0) * 100.0) if ours_s else 0.0:.3f}",
            "average_cell_advantage_pct": f"{statistics.mean(advantages):.3f}" if advantages else "0.000",
        })

    def sort_key(row: dict[str, object]) -> float:
        try:
            return float(row["value"])
        except ValueError:
            return math.inf

    return sorted(out, key=sort_key)


def md_table(headers: list[str], rows: list[list[object]]) -> list[str]:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(str(cell) for cell in row) + " |")
    return lines


def format_cell(row: dict[str, object], value_field: str = "ours_advantage_pct") -> str:
    return f"q{row['quality']} @ {float(row['target_mpix']):g} MP ({float(row[value_field]):+.3f})"


def write_summary_markdown(
    out: Path,
    cell_rows: list[dict[str, object]],
    by_quality: list[dict[str, object]],
    by_size: list[dict[str, object]],
    delta_rows: list[dict[str, object]] | None,
    baseline_matrix: Path | None,
) -> None:
    lines: list[str] = ["# Decoder quality/size matrix summary", ""]
    if not cell_rows:
        lines.extend(["No matrix cells were generated.", ""])
        out.write_text("\n".join(lines))
        return

    advantages = [row_float(r, "ours_advantage_pct") for r in cell_rows]
    labels = [faster_label(a) for a in advantages]
    best = sorted(cell_rows, key=lambda r: row_float(r, "ours_advantage_pct"), reverse=True)[:5]
    worst = sorted(cell_rows, key=lambda r: row_float(r, "ours_advantage_pct"))[:5]
    lines.extend([
        "## Current matrix",
        "",
        f"- Cells: {len(cell_rows)}",
        f"- Ours faster: {labels.count('ours')}",
        f"- libwebp faster: {labels.count('libwebp')}",
        f"- Tied: {labels.count('tie')}",
        f"- Best cell: {format_cell(best[0])}",
        f"- Worst cell: {format_cell(worst[0])}",
        "",
        "### Best cells by our advantage",
        "",
    ])
    lines.extend(md_table(["cell", "ours MP/s", "libwebp MP/s"], [
        [format_cell(r), r["ours_mp_s"], r["libwebp_mp_s"]] for r in best
    ]))
    lines.extend(["", "### Worst cells by our advantage", ""])
    lines.extend(md_table(["cell", "ours MP/s", "libwebp MP/s"], [
        [format_cell(r), r["ours_mp_s"], r["libwebp_mp_s"]] for r in worst
    ]))
    lines.extend(["", "### By quality", ""])
    lines.extend(md_table(["q", "cells", "ours faster", "libwebp faster", "advantage %", "ours MP/s", "libwebp MP/s"], [
        [
            f"q{r['value']}",
            r["cells"],
            r["ours_faster_cells"],
            r["libwebp_faster_cells"],
            r["ours_advantage_pct"],
            r["ours_mp_s"],
            r["libwebp_mp_s"],
        ] for r in by_quality
    ]))
    lines.extend(["", "### By target pixel size", ""])
    lines.extend(md_table(["target", "cells", "ours faster", "libwebp faster", "advantage %", "ours MP/s", "libwebp MP/s"], [
        [
            f"{float(r['value']):g} MP",
            r["cells"],
            r["ours_faster_cells"],
            r["libwebp_faster_cells"],
            r["ours_advantage_pct"],
            r["ours_mp_s"],
            r["libwebp_mp_s"],
        ] for r in by_size
    ]))

    if baseline_matrix is not None:
        lines.extend(["", "## Regression vs baseline", "", f"Baseline: `{baseline_matrix}`", ""])
        if not delta_rows:
            lines.extend(["No matching baseline cells were found.", ""])
        else:
            deltas = [row_float(r, "delta_advantage_pp") for r in delta_rows]
            improved = sum(1 for d in deltas if d > 0.0005)
            regressed = sum(1 for d in deltas if d < -0.0005)
            flat = len(deltas) - improved - regressed
            best_delta = sorted(delta_rows, key=lambda r: row_float(r, "delta_advantage_pp"), reverse=True)[:5]
            worst_delta = sorted(delta_rows, key=lambda r: row_float(r, "delta_advantage_pp"))[:5]
            lines.extend([
                f"- Matched cells: {len(delta_rows)}",
                f"- Missing baseline cells: {len(cell_rows) - len(delta_rows)}",
                f"- Improved advantage cells: {improved}",
                f"- Regressed advantage cells: {regressed}",
                f"- Unchanged cells: {flat}",
                f"- Best delta: {format_cell(best_delta[0], 'delta_advantage_pp')} pp",
                f"- Worst delta: {format_cell(worst_delta[0], 'delta_advantage_pp')} pp",
                "",
                "### Best advantage deltas",
                "",
            ])
            lines.extend(md_table(["cell", "delta pp", "current adv", "baseline adv", "ours MP/s delta"], [
                [
                    f"q{r['quality']} @ {float(r['target_mpix']):g} MP",
                    r["delta_advantage_pp"],
                    r["current_ours_advantage_pct"],
                    r["baseline_ours_advantage_pct"],
                    r["delta_ours_mp_s"],
                ] for r in best_delta
            ]))
            lines.extend(["", "### Worst advantage deltas", ""])
            lines.extend(md_table(["cell", "delta pp", "current adv", "baseline adv", "ours MP/s delta"], [
                [
                    f"q{r['quality']} @ {float(r['target_mpix']):g} MP",
                    r["delta_advantage_pp"],
                    r["current_ours_advantage_pct"],
                    r["baseline_ours_advantage_pct"],
                    r["delta_ours_mp_s"],
                ] for r in worst_delta
            ]))

    out.write_text("\n".join(lines) + "\n")


def write_heatmap_svg(cells: list[dict[str, object]], qualities: list[str], mpix_values: list[float], out: Path) -> None:
    cell_w = 98
    cell_h = 46
    left = 100
    top = 72
    width = left + cell_w * len(qualities) + 40
    height = top + cell_h * len(mpix_values) + 90
    by_key = {(str(c["quality"]), float(c["target_mpix"])): c for c in cells}
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="20" y="28" font-family="sans-serif" font-size="18" font-weight="700">Decoder speed advantage by WebP quality and pixel size</text>',
        '<text x="20" y="50" font-family="sans-serif" font-size="12">Green: ours faster. Red: libwebp faster. Cell text: signed advantage and our/libwebp MP/s.</text>',
    ]
    for ix, q in enumerate(qualities):
        x = left + ix * cell_w + cell_w / 2
        lines.append(f'<text x="{x:.1f}" y="{top - 18}" text-anchor="middle" font-family="sans-serif" font-size="12">q{html.escape(q)}</text>')
    for iy, mpix in enumerate(mpix_values):
        y = top + iy * cell_h
        lines.append(f'<text x="{left - 10}" y="{y + 25}" text-anchor="end" font-family="sans-serif" font-size="12">{mpix:g} MP</text>')
        for ix, q in enumerate(qualities):
            x = left + ix * cell_w
            c = by_key.get((q, mpix))
            if not c:
                fill = "rgb(238,238,238)"
                text1 = "n/a"
                text2 = ""
            else:
                adv = float(c["ours_advantage_pct"])
                fill = color_for_advantage(adv)
                text1 = f'{adv:+.0f}%'
                text2 = f'{float(c["ours_mp_s"]):.0f}/{float(c["libwebp_mp_s"]):.0f}'
            lines.append(f'<rect x="{x}" y="{y}" width="{cell_w - 2}" height="{cell_h - 2}" fill="{fill}" stroke="#fff"/>')
            lines.append(f'<text x="{x + cell_w / 2:.1f}" y="{y + 18}" text-anchor="middle" font-family="sans-serif" font-size="12" font-weight="700">{html.escape(text1)}</text>')
            lines.append(f'<text x="{x + cell_w / 2:.1f}" y="{y + 34}" text-anchor="middle" font-family="sans-serif" font-size="10">{html.escape(text2)}</text>')
    legend_y = top + cell_h * len(mpix_values) + 36
    lines.extend([
        f'<rect x="{left}" y="{legend_y}" width="24" height="14" fill="{color_for_advantage(40)}" stroke="#ccc"/>',
        f'<text x="{left + 32}" y="{legend_y + 12}" font-family="sans-serif" font-size="12">ours faster</text>',
        f'<rect x="{left + 140}" y="{legend_y}" width="24" height="14" fill="{color_for_advantage(-40)}" stroke="#ccc"/>',
        f'<text x="{left + 172}" y="{legend_y + 12}" font-family="sans-serif" font-size="12">libwebp faster</text>',
        '</svg>',
    ])
    out.write_text("\n".join(lines) + "\n")


def write_regression_heatmap_svg(cells: list[dict[str, object]], qualities: list[str], mpix_values: list[float], out: Path) -> None:
    cell_w = 112
    cell_h = 52
    left = 100
    top = 72
    width = left + cell_w * len(qualities) + 40
    height = top + cell_h * len(mpix_values) + 90
    by_key = {(str(c["quality"]), float(c["target_mpix"])): c for c in cells}
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="20" y="28" font-family="sans-serif" font-size="18" font-weight="700">Decoder regression delta by WebP quality and pixel size</text>',
        '<text x="20" y="50" font-family="sans-serif" font-size="12">Green: current advantage improved. Red: regressed. Cell text: advantage delta and current/baseline our MP/s.</text>',
    ]
    for ix, q in enumerate(qualities):
        x = left + ix * cell_w + cell_w / 2
        lines.append(f'<text x="{x:.1f}" y="{top - 18}" text-anchor="middle" font-family="sans-serif" font-size="12">q{html.escape(q)}</text>')
    for iy, mpix in enumerate(mpix_values):
        y = top + iy * cell_h
        lines.append(f'<text x="{left - 10}" y="{y + 28}" text-anchor="end" font-family="sans-serif" font-size="12">{mpix:g} MP</text>')
        for ix, q in enumerate(qualities):
            x = left + ix * cell_w
            c = by_key.get((q, mpix))
            if not c:
                fill = "rgb(238,238,238)"
                text1 = "n/a"
                text2 = ""
            else:
                delta = float(c["delta_advantage_pp"])
                fill = color_for_delta(delta)
                text1 = f'{delta:+.1f} pp'
                text2 = f'{float(c["current_ours_mp_s"]):.0f}/{float(c["baseline_ours_mp_s"]):.0f} MP/s'
            lines.append(f'<rect x="{x}" y="{y}" width="{cell_w - 2}" height="{cell_h - 2}" fill="{fill}" stroke="#fff"/>')
            lines.append(f'<text x="{x + cell_w / 2:.1f}" y="{y + 20}" text-anchor="middle" font-family="sans-serif" font-size="12" font-weight="700">{html.escape(text1)}</text>')
            lines.append(f'<text x="{x + cell_w / 2:.1f}" y="{y + 38}" text-anchor="middle" font-family="sans-serif" font-size="10">{html.escape(text2)}</text>')
    legend_y = top + cell_h * len(mpix_values) + 36
    lines.extend([
        f'<rect x="{left}" y="{legend_y}" width="24" height="14" fill="{color_for_delta(15)}" stroke="#ccc"/>',
        f'<text x="{left + 32}" y="{legend_y + 12}" font-family="sans-serif" font-size="12">improved</text>',
        f'<rect x="{left + 140}" y="{legend_y}" width="24" height="14" fill="{color_for_delta(-15)}" stroke="#ccc"/>',
        f'<text x="{left + 172}" y="{legend_y + 12}" font-family="sans-serif" font-size="12">regressed</text>',
        '</svg>',
    ])
    out.write_text("\n".join(lines) + "\n")


def write_heatmap_html(
    svg_name: str,
    out: Path,
    summary_name: str | None = None,
    regression_svg_name: str | None = None,
    title: str = "Decoder quality/size heatmap",
) -> None:
    links = ""
    if summary_name:
        links += f"<p>Summary: <a href='{html.escape(summary_name)}'>{html.escape(summary_name)}</a>.</p>"
    if regression_svg_name:
        links += f"<p>Regression delta heatmap: <a href='{html.escape(regression_svg_name)}'>{html.escape(regression_svg_name)}</a>.</p>"
    out.write_text(
        "<!doctype html><meta charset='utf-8'>"
        f"<title>{html.escape(title)}</title>"
        "<style>body{font-family:sans-serif;margin:24px;max-width:1200px}</style>"
        f"<h1>{html.escape(title)}</h1>"
        f"<p>Open the SVG directly or view it below: <a href='{html.escape(svg_name)}'>{html.escape(svg_name)}</a>.</p>"
        f"{links}"
        f"<img src='{html.escape(svg_name)}' alt='quality size heatmap'>\n"
    )


def write_summary_html(markdown_path: Path, out: Path) -> None:
    markdown = markdown_path.read_text()
    out.write_text(
        "<!doctype html><meta charset='utf-8'>"
        "<title>Decoder quality/size summary</title>"
        "<style>body{font-family:sans-serif;margin:24px;max-width:1200px}"
        "pre{white-space:pre-wrap;line-height:1.35}</style>"
        "<h1>Decoder quality/size summary</h1>"
        f"<p>Markdown source: <a href='{html.escape(markdown_path.name)}'>{html.escape(markdown_path.name)}</a>.</p>"
        f"<pre>{html.escape(markdown)}</pre>\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default="build/profile/quality-size-matrix")
    parser.add_argument("--decoder", default="build/decoder")
    parser.add_argument("--source-glob", action="append", default=["images/commons/*.jpg"])
    parser.add_argument("--qualities", default="0,10,20,30,40,50,60,70,80,90,100")
    parser.add_argument("--megapixels", default="0.25,0.5,1,2,4,8,16,32")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--force-generate", action="store_true")
    parser.add_argument("--skip-generate", action="store_true")
    parser.add_argument(
        "--baseline-matrix",
        help="Optional prior quality_size_matrix.csv to compare against the current run.",
    )
    args = parser.parse_args()

    root = repo_root()
    out_dir = root / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    decoder = root / args.decoder
    if not decoder.exists():
        print(f"decoder not found: {decoder}", file=sys.stderr)
        return 2
    if not shutil.which("magick"):
        print("magick not found", file=sys.stderr)
        return 2

    qualities_f = parse_csv_floats(args.qualities)
    qualities = [quality_label(q) for q in qualities_f]
    mpix_values = parse_csv_floats(args.megapixels)
    sources = identify_sources(args.source_glob)
    helper = build_libwebp_helper(out_dir)

    variants: list[Variant] = []
    for src in sources:
        for target_mpix in mpix_values:
            dims = target_dimensions(src, target_mpix)
            if not dims:
                continue
            width, height = dims
            size_dir = f"mp{target_mpix:g}".replace(".", "p")
            for q in qualities:
                mp_label = ("%g" % target_mpix).replace(".", "p")
                webp = out_dir / "corpus" / size_dir / f"{src.path.stem}-mp{mp_label}-q{q}.webp"
                variants.append(Variant(src, target_mpix, q, width, height, webp))

    if not args.skip_generate:
        for variant in variants:
            generate_variant(variant, args.force_generate)

    rows: list[dict[str, object]] = []
    for i, variant in enumerate(variants, 1):
        ours_cmd = [str(decoder), "-ppm", str(variant.webp), "/dev/null"]
        lib_cmd = [str(helper), "ppm", str(variant.webp), "/dev/null"]
        ours_median, ours_best, ours_worst = time_command(ours_cmd, args.warmups, args.runs)
        lib_median, lib_best, lib_worst = time_command(lib_cmd, args.warmups, args.runs)
        ours_mp_s = variant.megapixels / ours_median
        lib_mp_s = variant.megapixels / lib_median
        rows.append({
            "source": variant.source.path.name,
            "target_mpix": variant.target_mpix,
            "quality": variant.quality,
            "width": variant.width,
            "height": variant.height,
            "pixels": variant.pixels,
            "megapixels": f"{variant.megapixels:.6f}",
            "webp_bytes": variant.webp.stat().st_size,
            "ours_median_s": f"{ours_median:.9f}",
            "ours_best_s": f"{ours_best:.9f}",
            "ours_worst_s": f"{ours_worst:.9f}",
            "ours_mp_s": f"{ours_mp_s:.3f}",
            "libwebp_median_s": f"{lib_median:.9f}",
            "libwebp_best_s": f"{lib_best:.9f}",
            "libwebp_worst_s": f"{lib_worst:.9f}",
            "libwebp_mp_s": f"{lib_mp_s:.3f}",
            "ours_advantage_pct": f"{(lib_median / ours_median - 1.0) * 100.0:.3f}",
        })
        print(f"[{i}/{len(variants)}] {variant.webp.name}: ours {ours_mp_s:.1f} MP/s, libwebp {lib_mp_s:.1f} MP/s")

    per_file_csv = out_dir / "quality_size_per_file.csv"
    with per_file_csv.open("w", newline="") as f:
        fieldnames = list(rows[0].keys()) if rows else []
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    cell_rows: list[dict[str, object]] = []
    for target_mpix in mpix_values:
        for q in qualities:
            subset = [r for r in rows if float(r["target_mpix"]) == target_mpix and str(r["quality"]) == q]
            if not subset:
                continue
            mp = sum(float(r["megapixels"]) for r in subset)
            ours_s = sum(float(r["ours_median_s"]) for r in subset)
            lib_s = sum(float(r["libwebp_median_s"]) for r in subset)
            cell_rows.append({
                "target_mpix": target_mpix,
                "quality": q,
                "files": len(subset),
                "megapixels": f"{mp:.6f}",
                "ours_seconds": f"{ours_s:.9f}",
                "libwebp_seconds": f"{lib_s:.9f}",
                "ours_mp_s": f"{mp / ours_s:.3f}",
                "libwebp_mp_s": f"{mp / lib_s:.3f}",
                "ours_advantage_pct": f"{(lib_s / ours_s - 1.0) * 100.0:.3f}",
            })

    matrix_csv = out_dir / "quality_size_matrix.csv"
    with matrix_csv.open("w", newline="") as f:
        fieldnames = list(cell_rows[0].keys()) if cell_rows else []
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(cell_rows)

    summary_fields = [
        "dimension",
        "value",
        "cells",
        "ours_faster_cells",
        "libwebp_faster_cells",
        "tied_cells",
        "megapixels",
        "ours_seconds",
        "libwebp_seconds",
        "ours_mp_s",
        "libwebp_mp_s",
        "ours_advantage_pct",
        "average_cell_advantage_pct",
    ]
    by_quality_rows = aggregate_cells(cell_rows, "quality")
    by_size_rows = aggregate_cells(cell_rows, "target_mpix")
    by_quality_csv = out_dir / "quality_size_by_quality.csv"
    by_size_csv = out_dir / "quality_size_by_size.csv"
    write_dict_csv(by_quality_csv, by_quality_rows, summary_fields)
    write_dict_csv(by_size_csv, by_size_rows, summary_fields)

    baseline_matrix: Path | None = None
    delta_rows: list[dict[str, object]] | None = None
    delta_csv: Path | None = None
    regression_svg: Path | None = None
    regression_html: Path | None = None
    if args.baseline_matrix:
        baseline_matrix = Path(args.baseline_matrix)
        if not baseline_matrix.is_absolute():
            baseline_matrix = root / baseline_matrix
        try:
            baseline_rows = load_matrix_csv(baseline_matrix)
        except (OSError, ValueError) as exc:
            print(f"failed to load baseline matrix: {exc}", file=sys.stderr)
            return 2
        delta_rows = compute_regression_rows(cell_rows, baseline_rows)
        delta_fields = [
            "target_mpix",
            "quality",
            "current_files",
            "baseline_files",
            "current_ours_advantage_pct",
            "baseline_ours_advantage_pct",
            "delta_advantage_pp",
            "current_ours_mp_s",
            "baseline_ours_mp_s",
            "delta_ours_mp_s",
            "delta_ours_mp_s_pct",
            "current_libwebp_mp_s",
            "baseline_libwebp_mp_s",
            "delta_libwebp_mp_s",
            "delta_libwebp_mp_s_pct",
            "current_faster",
            "baseline_faster",
        ]
        delta_csv = out_dir / "quality_size_regression_delta.csv"
        write_dict_csv(delta_csv, delta_rows, delta_fields)
        regression_svg = out_dir / "quality_size_regression_heatmap.svg"
        write_regression_heatmap_svg(delta_rows, qualities, mpix_values, regression_svg)
        regression_html = out_dir / "quality_size_regression_heatmap.html"

    summary_md = out_dir / "quality_size_summary.md"
    write_summary_markdown(summary_md, cell_rows, by_quality_rows, by_size_rows, delta_rows, baseline_matrix)
    summary_html = out_dir / "quality_size_summary.html"
    write_summary_html(summary_md, summary_html)

    svg = out_dir / "quality_size_heatmap.svg"
    write_heatmap_svg(cell_rows, qualities, mpix_values, svg)
    write_heatmap_html(
        svg.name,
        out_dir / "quality_size_heatmap.html",
        summary_name=summary_html.name,
        regression_svg_name=regression_svg.name if regression_svg else None,
    )
    if regression_svg and regression_html:
        write_heatmap_html(
            regression_svg.name,
            regression_html,
            summary_name=summary_html.name,
            title="Decoder quality/size regression heatmap",
        )

    print(f"wrote {per_file_csv}")
    print(f"wrote {matrix_csv}")
    print(f"wrote {by_quality_csv}")
    print(f"wrote {by_size_csv}")
    print(f"wrote {summary_md}")
    print(f"wrote {summary_html}")
    if delta_csv:
        print(f"wrote {delta_csv}")
    if regression_svg:
        print(f"wrote {regression_svg}")
    if regression_html:
        print(f"wrote {regression_html}")
    print(f"wrote {svg}")
    print(f"wrote {out_dir / 'quality_size_heatmap.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
