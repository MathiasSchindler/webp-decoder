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


def write_heatmap_html(svg_name: str, out: Path) -> None:
    out.write_text(
        "<!doctype html><meta charset='utf-8'>"
        "<title>Decoder quality/size heatmap</title>"
        "<style>body{font-family:sans-serif;margin:24px;max-width:1200px}</style>"
        "<h1>Decoder quality/size heatmap</h1>"
        f"<p>Open the SVG directly or view it below: <a href='{html.escape(svg_name)}'>{html.escape(svg_name)}</a>.</p>"
        f"<img src='{html.escape(svg_name)}' alt='quality size heatmap'>\n"
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

    svg = out_dir / "quality_size_heatmap.svg"
    write_heatmap_svg(cell_rows, qualities, mpix_values, svg)
    write_heatmap_html(svg.name, out_dir / "quality_size_heatmap.html")

    print(f"wrote {per_file_csv}")
    print(f"wrote {matrix_csv}")
    print(f"wrote {svg}")
    print(f"wrote {out_dir / 'quality_size_heatmap.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
