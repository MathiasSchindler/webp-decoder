#!/usr/bin/env python3
import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class Point:
    x: float
    y: float


def _is_finite(x: float) -> bool:
    return not (math.isnan(x) or math.isinf(x))


def _try_float(s: str) -> Optional[float]:
    try:
        return float(s)
    except Exception:
        return None


def _nice_ticks(vmin: float, vmax: float, target_ticks: int = 6) -> List[float]:
    if vmax <= vmin:
        return [vmin]
    span = vmax - vmin
    raw_step = span / max(1, target_ticks - 1)
    mag = 10 ** math.floor(math.log10(raw_step))
    for m in (1, 2, 5, 10):
        step = m * mag
        if span / step <= target_ticks + 0.5:
            break
    start = math.floor(vmin / step) * step
    ticks = []
    t = start
    # Guard against infinite loops via max iterations
    for _ in range(10_000):
        if t > vmax + 1e-12:
            break
        if t >= vmin - 1e-12:
            ticks.append(t)
        t += step
    return ticks


def _fmt_tick(v: float) -> str:
    if abs(v) >= 100:
        return f"{v:.0f}"
    if abs(v) >= 10:
        return f"{v:.1f}"
    if abs(v) >= 1:
        return f"{v:.2f}"
    return f"{v:.3f}"


def load_points_best_per_quality(csv_path: Path) -> Tuple[List[Point], List[Point]]:
    # For "our": pick min butteraugli per quality across modes.
    our_best: Dict[int, Tuple[float, float]] = {}
    cwebp: Dict[int, Tuple[float, float]] = {}

    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            enc = (row.get("encoder") or "").strip()
            q = _try_float((row.get("quality") or "").strip())
            size = _try_float((row.get("webp_bytes") or "").strip())
            score = _try_float((row.get("butteraugli") or "").strip())
            if q is None or size is None or score is None:
                continue
            if not (_is_finite(size) and _is_finite(score)):
                continue
            qi = int(round(q))

            if enc == "our":
                prev = our_best.get(qi)
                if prev is None or score < prev[1]:
                    our_best[qi] = (size, score)
            elif enc == "cwebp":
                # usually one point per q; keep best score if duplicates
                prev = cwebp.get(qi)
                if prev is None or score < prev[1]:
                    cwebp[qi] = (size, score)

    our_points = [Point(x=v[0], y=v[1]) for _, v in sorted(our_best.items())]
    cwebp_points = [Point(x=v[0], y=v[1]) for _, v in sorted(cwebp.items())]
    return our_points, cwebp_points


def render_svg(
    our: List[Point],
    cwebp: List[Point],
    title: str,
    out_path: Path,
    width: int = 980,
    height: int = 560,
) -> None:
    all_pts = our + cwebp
    if not all_pts:
        out_path.write_text(
            "<svg xmlns='http://www.w3.org/2000/svg' width='800' height='200'><text x='10' y='30'>No data to plot.</text></svg>\n",
            encoding="utf-8",
        )
        return

    xmin = min(p.x for p in all_pts)
    xmax = max(p.x for p in all_pts)
    ymin = min(p.y for p in all_pts)
    ymax = max(p.y for p in all_pts)

    # Add padding
    xpad = (xmax - xmin) * 0.05 if xmax > xmin else 1.0
    ypad = (ymax - ymin) * 0.08 if ymax > ymin else 1.0
    xmin -= xpad
    xmax += xpad
    ymin = max(0.0, ymin - ypad)
    ymax += ypad

    margin_l, margin_r, margin_t, margin_b = 78, 18, 44, 64
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    def sx(x: float) -> float:
        if xmax == xmin:
            return margin_l + plot_w / 2
        return margin_l + (x - xmin) * plot_w / (xmax - xmin)

    def sy(y: float) -> float:
        if ymax == ymin:
            return margin_t + plot_h / 2
        # SVG y goes downward
        return margin_t + (ymax - y) * plot_h / (ymax - ymin)

    xticks = _nice_ticks(xmin, xmax, target_ticks=7)
    yticks = _nice_ticks(ymin, ymax, target_ticks=7)

    def poly(points: List[Point]) -> str:
        pts = " ".join(f"{sx(p.x):.2f},{sy(p.y):.2f}" for p in sorted(points, key=lambda p: p.x))
        return pts

    def dots(points: List[Point], color: str) -> str:
        parts = []
        for p in points:
            parts.append(
                f"<circle cx='{sx(p.x):.2f}' cy='{sy(p.y):.2f}' r='2.6' fill='{color}' opacity='0.95'/>"
            )
        return "\n".join(parts)

    # Colors (high contrast, color-blind friendly-ish)
    c_our = "#0072B2"   # blue
    c_cwebp = "#D55E00" # vermillion

    svg = []
    svg.append(f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' viewBox='0 0 {width} {height}'>")
    svg.append("<rect x='0' y='0' width='100%' height='100%' fill='white'/>")
    svg.append(
        "<style>"
        ".axis{stroke:#333;stroke-width:1.2}"
        ".grid{stroke:#ddd;stroke-width:1}"
        ".label{font:12px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial;}"
        ".title{font:16px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial;font-weight:600}"
        ".legend{font:12px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial;}"
        "</style>"
    )

    # Title
    svg.append(f"<text class='title' x='{margin_l}' y='26'>{title}</text>")

    # Grid + ticks
    for t in xticks:
        x = sx(t)
        svg.append(f"<line class='grid' x1='{x:.2f}' y1='{margin_t}' x2='{x:.2f}' y2='{margin_t + plot_h}'/>")
        svg.append(f"<text class='label' x='{x:.2f}' y='{margin_t + plot_h + 20}' text-anchor='middle'>{_fmt_tick(t)}</text>")

    for t in yticks:
        y = sy(t)
        svg.append(f"<line class='grid' x1='{margin_l}' y1='{y:.2f}' x2='{margin_l + plot_w}' y2='{y:.2f}'/>")
        svg.append(f"<text class='label' x='{margin_l - 10}' y='{y + 4:.2f}' text-anchor='end'>{_fmt_tick(t)}</text>")

    # Axes
    svg.append(f"<line class='axis' x1='{margin_l}' y1='{margin_t}' x2='{margin_l}' y2='{margin_t + plot_h}'/>")
    svg.append(f"<line class='axis' x1='{margin_l}' y1='{margin_t + plot_h}' x2='{margin_l + plot_w}' y2='{margin_t + plot_h}'/>")

    # Axis labels
    svg.append(
        f"<text class='label' x='{margin_l + plot_w / 2:.2f}' y='{height - 22}' text-anchor='middle'>WebP size (bytes)</text>"
    )
    svg.append(
        f"<text class='label' x='20' y='{margin_t + plot_h / 2:.2f}' text-anchor='middle' transform='rotate(-90 20 {margin_t + plot_h / 2:.2f})'>Butteraugli (lower is better)</text>"
    )

    # Lines + points
    if cwebp:
        svg.append(f"<polyline fill='none' stroke='{c_cwebp}' stroke-width='2.0' points='{poly(cwebp)}' opacity='0.9'/>")
        svg.append(dots(cwebp, c_cwebp))
    if our:
        svg.append(f"<polyline fill='none' stroke='{c_our}' stroke-width='2.0' points='{poly(our)}' opacity='0.9'/>")
        svg.append(dots(our, c_our))

    # Legend
    lx = margin_l + 8
    ly = margin_t + 8
    svg.append(f"<rect x='{lx - 6}' y='{ly - 14}' width='210' height='44' fill='white' opacity='0.85' stroke='#ddd'/>")
    svg.append(f"<line x1='{lx}' y1='{ly}' x2='{lx + 24}' y2='{ly}' stroke='{c_our}' stroke-width='3'/>")
    svg.append(f"<text class='legend' x='{lx + 30}' y='{ly + 4}'>encoder (best per -q)</text>")
    svg.append(f"<line x1='{lx}' y1='{ly + 18}' x2='{lx + 24}' y2='{ly + 18}' stroke='{c_cwebp}' stroke-width='3'/>")
    svg.append(f"<text class='legend' x='{lx + 30}' y='{ly + 22}'>cwebp</text>")

    svg.append("</svg>\n")
    out_path.write_text("\n".join(svg), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--title", default="Butteraugli vs WebP size")
    args = ap.parse_args()

    our, cwebp = load_points_best_per_quality(args.input)
    render_svg(our=our, cwebp=cwebp, title=args.title, out_path=args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
