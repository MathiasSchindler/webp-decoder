#!/usr/bin/env python3
"""Plot RD curves (bytes vs SSIM/PSNR) to SVG.

No external dependencies: uses only the Python standard library.

Input: TSV produced by scripts/enc_vs_cwebp_rdcurve.sh
Columns:
  image\tsize\tencoder\tq\tbytes\tpsnr_rgb\tssim_y

Output: writes two SVGs next to the input file by default.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
from dataclasses import dataclass
from typing import Dict, Iterable, List, Tuple


@dataclass(frozen=True)
class Point:
    q: int
    bytes: int
    psnr: float
    ssim: float


def _parse_float(s: str) -> float:
    s = s.strip()
    if s == "inf":
        return math.inf
    if s == "nan":
        return math.nan
    return float(s)


def _read_points(path: str) -> Dict[str, List[Point]]:
    by_encoder: Dict[str, List[Point]] = {}
    with open(path, "r", encoding="utf-8") as fp:
        reader = csv.DictReader(fp, delimiter="\t")
        required = {"encoder", "q", "bytes", "psnr_rgb", "ssim_y"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise SystemExit(f"TSV missing columns: {sorted(missing)}")
        for row in reader:
            enc = row["encoder"].strip()
            by_encoder.setdefault(enc, []).append(
                Point(
                    q=int(row["q"]),
                    bytes=int(row["bytes"]),
                    psnr=_parse_float(row["psnr_rgb"]),
                    ssim=_parse_float(row["ssim_y"]),
                )
            )
    for enc, pts in by_encoder.items():
        pts.sort(key=lambda p: p.bytes)
    return by_encoder


def _finite(vals: Iterable[float]) -> List[float]:
    out: List[float] = []
    for v in vals:
        if math.isfinite(v):
            out.append(v)
    return out


def _svg_escape(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


def _map_linear(x: float, x0: float, x1: float, y0: float, y1: float) -> float:
    if x1 == x0:
        return (y0 + y1) * 0.5
    t = (x - x0) / (x1 - x0)
    return y0 + t * (y1 - y0)


def _polyline(points_xy: List[Tuple[float, float]]) -> str:
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points_xy)


def _format_num(x: float) -> str:
    if abs(x) >= 100:
        return f"{x:.0f}"
    if abs(x) >= 10:
        return f"{x:.1f}"
    return f"{x:.3f}"


def render_svg(
    title: str,
    x_label: str,
    y_label: str,
    ours: List[Point],
    lib: List[Point],
    y_metric: str,
    out_path: str,
) -> None:
    width = 960
    height = 640
    margin_l = 80
    margin_r = 20
    margin_t = 50
    margin_b = 70

    def y_of(p: Point) -> float:
        return getattr(p, y_metric)

    all_points = ours + lib
    x_vals = [float(p.bytes) for p in all_points]
    y_vals = _finite([float(y_of(p)) for p in all_points])

    if not x_vals or not y_vals:
        raise SystemExit("No points to plot")

    x_min = min(x_vals)
    x_max = max(x_vals)
    if x_min == x_max:
        x_max = x_min + 1.0

    y_min = min(y_vals)
    y_max = max(y_vals)
    if y_min == y_max:
        y_max = y_min + 1.0

    # Pad a bit so lines aren't on the border.
    x_pad = (x_max - x_min) * 0.03
    y_pad = (y_max - y_min) * 0.06
    x_min -= x_pad
    x_max += x_pad
    y_min -= y_pad
    y_max += y_pad

    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    def px(x: float) -> float:
        return _map_linear(x, x_min, x_max, margin_l, margin_l + plot_w)

    def py(y: float) -> float:
        # SVG y grows down.
        return _map_linear(y, y_min, y_max, margin_t + plot_h, margin_t)

    def to_xy(points: List[Point]) -> List[Tuple[float, float]]:
        out: List[Tuple[float, float]] = []
        for p in points:
            yv = float(y_of(p))
            if not math.isfinite(yv):
                continue
            out.append((px(float(p.bytes)), py(yv)))
        return out

    ours_xy = to_xy(ours)
    lib_xy = to_xy(lib)

    # Axis ticks (simple): 6 ticks.
    def ticks(v0: float, v1: float, n: int) -> List[float]:
        if n <= 1:
            return [v0]
        step = (v1 - v0) / (n - 1)
        return [v0 + i * step for i in range(n)]

    x_ticks = ticks(x_min + x_pad, x_max - x_pad, 6)
    y_ticks = ticks(y_min + y_pad, y_max - y_pad, 6)

    # Colors.
    ours_color = "#d62728"  # red
    lib_color = "#1f77b4"  # blue
    grid_color = "#dddddd"
    axis_color = "#333333"

    svg_lines: List[str] = []
    svg_lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">')
    svg_lines.append('<rect x="0" y="0" width="100%" height="100%" fill="white"/>')

    # Title.
    svg_lines.append(
        f'<text x="{width/2:.1f}" y="{margin_t-18}" text-anchor="middle" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="18" fill="#111">{_svg_escape(title)}</text>'
    )

    # Grid + ticks.
    for xt in x_ticks:
        x0 = px(xt)
        svg_lines.append(f'<line x1="{x0:.2f}" y1="{margin_t}" x2="{x0:.2f}" y2="{margin_t+plot_h}" stroke="{grid_color}" stroke-width="1"/>')
        svg_lines.append(
            f'<text x="{x0:.2f}" y="{margin_t+plot_h+18}" text-anchor="middle" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="12" fill="#333">{_svg_escape(str(int(round(xt))))}</text>'
        )

    for yt in y_ticks:
        y0 = py(yt)
        svg_lines.append(f'<line x1="{margin_l}" y1="{y0:.2f}" x2="{margin_l+plot_w}" y2="{y0:.2f}" stroke="{grid_color}" stroke-width="1"/>')
        svg_lines.append(
            f'<text x="{margin_l-10}" y="{y0+4:.2f}" text-anchor="end" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="12" fill="#333">{_svg_escape(_format_num(yt))}</text>'
        )

    # Axes.
    svg_lines.append(f'<rect x="{margin_l}" y="{margin_t}" width="{plot_w}" height="{plot_h}" fill="none" stroke="{axis_color}" stroke-width="1.5"/>')

    # Labels.
    svg_lines.append(
        f'<text x="{width/2:.1f}" y="{height-22}" text-anchor="middle" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="14" fill="#111">{_svg_escape(x_label)}</text>'
    )
    svg_lines.append(
        f'<text x="18" y="{height/2:.1f}" transform="rotate(-90 18 {height/2:.1f})" text-anchor="middle" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="14" fill="#111">{_svg_escape(y_label)}</text>'
    )

    # Curves.
    if lib_xy:
        svg_lines.append(f'<polyline fill="none" stroke="{lib_color}" stroke-width="2" points="{_polyline(lib_xy)}"/>')
        for x, y in lib_xy:
            svg_lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="2.2" fill="{lib_color}"/>')

    if ours_xy:
        svg_lines.append(f'<polyline fill="none" stroke="{ours_color}" stroke-width="2" points="{_polyline(ours_xy)}"/>')
        for x, y in ours_xy:
            svg_lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="2.2" fill="{ours_color}"/>')

    # Legend.
    legend_x = margin_l + 12
    legend_y = margin_t + 14
    svg_lines.append(f'<rect x="{legend_x-8}" y="{legend_y-12}" width="210" height="46" fill="white" opacity="0.9" stroke="#ccc"/>')
    svg_lines.append(f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x+18}" y2="{legend_y}" stroke="{lib_color}" stroke-width="3"/>')
    svg_lines.append(
        f'<text x="{legend_x+24}" y="{legend_y+4}" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="12" fill="#111">libwebp (cwebp)</text>'
    )
    svg_lines.append(f'<line x1="{legend_x}" y1="{legend_y+18}" x2="{legend_x+18}" y2="{legend_y+18}" stroke="{ours_color}" stroke-width="3"/>')
    svg_lines.append(
        f'<text x="{legend_x+24}" y="{legend_y+22}" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif" font-size="12" fill="#111">ours (./encoder)</text>'
    )

    svg_lines.append("</svg>\n")

    with open(out_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(svg_lines))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv", help="Input TSV from enc_vs_cwebp_rdcurve.sh")
    ap.add_argument("--out-dir", default=None, help="Output directory (default: alongside TSV)")
    ap.add_argument("--title", default=None, help="SVG title (default: derived from TSV filename)")
    args = ap.parse_args()

    by_enc = _read_points(args.tsv)
    ours = by_enc.get("ours", [])
    lib = by_enc.get("lib", [])
    if not ours or not lib:
        raise SystemExit(f"Need both encoders in TSV; got: {sorted(by_enc.keys())}")

    out_dir = args.out_dir or os.path.dirname(os.path.abspath(args.tsv))
    base = os.path.splitext(os.path.basename(args.tsv))[0]

    title = args.title or base

    render_svg(
        title=f"{title} — SSIM(Y) vs bytes",
        x_label="Output size (bytes)",
        y_label="SSIM (luma, Y)",
        ours=ours,
        lib=lib,
        y_metric="ssim",
        out_path=os.path.join(out_dir, f"{base}_ssim.svg"),
    )

    render_svg(
        title=f"{title} — PSNR(RGB) vs bytes",
        x_label="Output size (bytes)",
        y_label="PSNR (RGB, dB)",
        ours=ours,
        lib=lib,
        y_metric="psnr",
        out_path=os.path.join(out_dir, f"{base}_psnr.svg"),
    )


if __name__ == "__main__":
    main()
