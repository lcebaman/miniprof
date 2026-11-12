"""
miniprof_html.py — one self-contained HTML page from a miniprof snapshot.

Used by miniprof_report.py --html. No external files, fonts or scripts:
inline CSS, inline SVG charts and a few lines of script for tooltips, so
the page opens offline on a login node or from an e-mail attachment.
Every chart has its numbers in a table next to it.

Colours: a validated categorical palette (tools/dataviz rules): slots 1-4
in fixed order, light and dark steps chosen separately; text never takes
a series colour; status colours only for observations, with an icon.
"""

import html
import math

# ---------------------------------------------------------------- helpers

def esc(s) -> str:
    return html.escape("" if s is None else str(s), quote=True)


def hb(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB", "PiB"):
        if abs(n) < 1024 or unit == "PiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} PiB"


def ht(s: float) -> str:
    if s is None:
        return "–"
    if s >= 100:
        return f"{s:.0f} s"
    if s >= 1:
        return f"{s:.2f} s"
    if s >= 1e-3:
        return f"{s * 1e3:.1f} ms"
    return f"{s * 1e6:.0f} µs"


def num(x, fmt="{:.1f}", none="–"):
    return none if x is None else fmt.format(x)


def gflops(x):
    if x is None:
        return "–"
    return f"{x / 1000:.2f} TFLOP/s" if x >= 1000 else f"{x:.1f} GFLOP/s"


def nice_max(v: float) -> float:
    """A round axis maximum >= v."""
    if v <= 0:
        return 1.0
    e = 10 ** math.floor(math.log10(v))
    for f in (1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if f * e >= v:
            return f * e
    return 10 * e


def axis(vmax: float, n: int = 4):
    """A round axis maximum and its ticks: steps of 1, 2, 2.5 or 5 x 10^k."""
    if vmax <= 0:
        return 1.0, [0.0, 0.25, 0.5, 0.75, 1.0]
    raw = vmax / n
    e = 10 ** math.floor(math.log10(raw))
    step = next(f * e for f in (1, 2, 2.5, 5, 10) if f * e >= raw)
    top = math.ceil(vmax / step - 1e-9) * step
    k = int(round(top / step))
    return top, [i * step for i in range(k + 1)]


def tick_label(v: float, unit: str = "") -> str:
    if v == 0:
        return "0"
    if abs(v) >= 1000:
        s = f"{v / 1000:g}k"
    elif abs(v) >= 10:
        s = f"{v:.0f}"
    elif abs(v) >= 1:
        s = f"{v:.1f}".rstrip("0").rstrip(".")
    else:
        s = f"{v:.2g}"
    return s + unit


def bar_path(x, y, w, h, r=4, end="top"):
    """A bar with only its data end rounded (end: top | right)."""
    if w <= 0 or h <= 0:
        return ""
    r = max(0.0, min(r, w / 2 if end == "top" else h / 2, h if end == "top" else w))
    if r < 0.5:
        return f"M{x:.2f},{y:.2f}h{w:.2f}v{h:.2f}h{-w:.2f}z"
    if end == "top":
        return (f"M{x:.2f},{y + h:.2f}V{y + r:.2f}Q{x:.2f},{y:.2f} {x + r:.2f},{y:.2f}"
                f"H{x + w - r:.2f}Q{x + w:.2f},{y:.2f} {x + w:.2f},{y + r:.2f}V{y + h:.2f}z")
    return (f"M{x:.2f},{y:.2f}H{x + w - r:.2f}Q{x + w:.2f},{y:.2f} {x + w:.2f},{y + r:.2f}"
            f"V{y + h - r:.2f}Q{x + w:.2f},{y + h:.2f} {x + w - r:.2f},{y + h:.2f}H{x:.2f}z")


def table(head, rows, cls="", text=()) -> str:
    """Columns after the first are right-aligned numbers unless listed in text."""
    isnum = lambda i: i > 0 and i not in text
    out = [f'<table class="{cls}"><thead><tr>']
    out += [f'<th{" class=num" if isnum(i) else ""}>{esc(h)}</th>' for i, h in enumerate(head)]
    out.append("</tr></thead><tbody>")
    for r in rows:
        out.append("<tr>" + "".join(
            f'<td{" class=num" if isnum(i) else ""}>{c if isinstance(c, Raw) else esc(c)}</td>'
            for i, c in enumerate(r)) + "</tr>")
    out.append("</tbody></table>")
    return "".join(out)


class Raw(str):
    """Already-escaped HTML for a table cell."""


def legend(items) -> str:
    return '<div class="legend">' + "".join(
        f'<span><i class="sw" style="background:var(--s{k})"></i>{esc(n)}</span>' for k, n in items) + "</div>"


def details_table(head, rows) -> str:
    return f'<details class="tv"><summary>Table</summary>{table(head, rows)}</details>'


def tile(label, value, sub="") -> str:
    return (f'<div class="tile"><div class="tl">{esc(label)}</div><div class="tv2">{esc(value)}</div>'
            + (f'<div class="ts">{esc(sub)}</div>' if sub else "") + "</div>")


# ---------------------------------------------------------------- charts

def hstack(rows, keys, width=960, bar_h=22, label_w=150, unit="%", vmax=None, tail_w=110):
    """Horizontal stacked bars: rows = [(label, {key: value}, tip)], keys =
    [(slot, key, name)]. 2px surface gap between segments; the last
    non-zero segment gets the rounded end."""
    gap = 10
    plot_w = width - label_w - tail_w
    if vmax is None:
        vmax = max([sum(v.get(k, 0) or 0 for _, k, _ in keys) for _, v, _ in rows] + [1e-9])
    h = len(rows) * (bar_h + gap) + 4
    out = [f'<svg class="chart" viewBox="0 0 {width} {h}" role="img">']
    for i, (label, vals, tip) in enumerate(rows):
        y = i * (bar_h + gap) + 2
        out.append(f'<text class="lab" x="{label_w - 8}" y="{y + bar_h / 2 + 4}" text-anchor="end">{esc(label)}</text>')
        x = label_w
        segs = [(s, k, n, vals.get(k) or 0) for s, k, n in keys]
        last = max([j for j, sg in enumerate(segs) if sg[3] > 0] + [-1])
        total = 0.0
        for j, (slot, k, name, v) in enumerate(segs):
            if v <= 0:
                continue
            w = v / vmax * plot_w
            total += v
            d = bar_path(x, y, max(w - (2 if j != last else 0), 0.5), bar_h, 4 if j == last else 0, "right")
            out.append(f'<path d="{d}" fill="var(--s{slot})" data-tip="{esc(label)} — {esc(name)}: '
                       f'{v:.1f}{esc(unit)}"/>')
            x += w
        if tip != "":
            out.append(f'<text class="val" x="{x + 6:.1f}" y="{y + bar_h / 2 + 4}">'
                       f'{esc(tip if tip is not None else f"{total:.1f}{unit}")}</text>')
    out.append("</svg>")
    return "".join(out)


def ranked_bars(rows, width=960, bar_h=18, label_w=170, unit="%"):
    """One series, ranked horizontal bars: rows = [(label, value, tip)]."""
    gap = 8
    plot_w = width - label_w - 70
    vmax = nice_max(max([v for _, v, _ in rows] + [1e-9]))
    h = len(rows) * (bar_h + gap) + 4
    out = [f'<svg class="chart" viewBox="0 0 {width} {h}" role="img">']
    for i, (label, v, tip) in enumerate(rows):
        y = i * (bar_h + gap) + 2
        w = max(v / vmax * plot_w, 0.5)
        out.append(f'<text class="lab" x="{label_w - 8}" y="{y + bar_h / 2 + 4}" text-anchor="end">{esc(label)}</text>')
        out.append(f'<rect class="hit" x="{label_w}" y="{y - gap / 2}" width="{plot_w}" height="{bar_h + gap}" '
                   f'data-tip="{esc(tip)}"/>')
        out.append(f'<path d="{bar_path(label_w, y, w, bar_h, 4, "right")}" fill="var(--s1)" data-tip="{esc(tip)}"/>')
        out.append(f'<text class="val" x="{label_w + w + 6:.1f}" y="{y + bar_h / 2 + 4}">{v:.1f}{esc(unit)}</text>')
    out.append("</svg>")
    return "".join(out)


def columns(values, labels, width=960, height=180, unit="", tipfmt=None, every=None):
    """One series, vertical columns with a y axis: values per category."""
    ml, mr, mt, mb = 44, 8, 8, 34
    pw, ph = width - ml - mr, height - mt - mb
    n = len(values)
    vmax, tks = axis(max(values + [1e-12]))
    out = [f'<svg class="chart" viewBox="0 0 {width} {height}" role="img">']
    for t in tks:
        y = mt + ph - t / vmax * ph
        out.append(f'<line class="grid" x1="{ml}" x2="{ml + pw}" y1="{y:.1f}" y2="{y:.1f}"/>'
                   f'<text class="tick" x="{ml - 6}" y="{y + 4:.1f}" text-anchor="end">{tick_label(t, unit)}</text>')
    slot = pw / max(n, 1)
    g = 2 if slot >= 6 else 0
    every = every or max(1, math.ceil(n / 12))
    for i, v in enumerate(values):
        x = ml + i * slot
        hgt = v / vmax * ph
        tip = tipfmt(i, v) if tipfmt else f"{labels[i]}: {v}"
        out.append(f'<rect class="hit" x="{x:.1f}" y="{mt}" width="{slot:.1f}" height="{ph}" data-tip="{esc(tip)}"/>')
        if hgt > 0:
            out.append(f'<path d="{bar_path(x + g / 2, mt + ph - hgt, slot - g, hgt, 4 if slot >= 8 else 0)}" '
                       f'fill="var(--s1)" data-tip="{esc(tip)}"/>')
        if i % every == 0:
            out.append(f'<text class="tick" x="{x + slot / 2:.1f}" y="{mt + ph + 14}" text-anchor="middle">'
                       f'{esc(labels[i])}</text>')
    out.append(f'<line class="axis" x1="{ml}" x2="{ml + pw}" y1="{mt + ph}" y2="{mt + ph}"/>')
    out.append("</svg>")
    return "".join(out)


def rank_chart(ranks, width=960, height=240):
    """Per-rank stacked columns: compute / MPI / I/O seconds. Ranks are
    averaged in consecutive groups above 512."""
    ml, mr, mt, mb = 48, 8, 8, 30
    pw, ph = width - ml - mr, height - mt - mb
    rs = sorted(ranks, key=lambda r: r["rank"])
    group = max(1, math.ceil(len(rs) / 512))
    cols = []
    for i in range(0, len(rs), group):
        g = rs[i:i + group]
        k = len(g)
        cols.append((g[0]["rank"], g[-1]["rank"],
                     sum(r.get("compute_s") or 0 for r in g) / k,
                     sum(r.get("mpi_s") or 0 for r in g) / k,
                     sum(r.get("io_s") or 0 for r in g) / k))
    vmax, tks = axis(max(c + m + io for _, _, c, m, io in cols))
    out = [f'<svg class="chart" viewBox="0 0 {width} {height}" role="img">']
    for t in tks:
        y = mt + ph - t / vmax * ph
        out.append(f'<line class="grid" x1="{ml}" x2="{ml + pw}" y1="{y:.1f}" y2="{y:.1f}"/>'
                   f'<text class="tick" x="{ml - 6}" y="{y + 4:.1f}" text-anchor="end">{tick_label(t, " s")}</text>')
    slot = pw / len(cols)
    g = 2 if slot >= 6 else (1 if slot >= 3 else 0)
    every = max(1, math.ceil(len(cols) / 16))
    for i, (a, b, c, m, io) in enumerate(cols):
        x = ml + i * slot
        who = f"rank {a}" if a == b else f"ranks {a}–{b} (average)"
        tip = f"{who}: compute {ht(c)}, MPI {ht(m)}, I/O {ht(io)}"
        out.append(f'<rect class="hit" x="{x:.1f}" y="{mt}" width="{slot:.1f}" height="{ph}" data-tip="{esc(tip)}"/>')
        y = mt + ph
        segs = [(1, c), (2, m), (3, io)]
        last = max([j for j, (_, v) in enumerate(segs) if v > 0] + [-1])
        for j, (sl, v) in enumerate(segs):
            if v <= 0:
                continue
            hgt = v / vmax * ph
            top_gap = 2 if (j != last and hgt > 3) else 0
            d = bar_path(x + g / 2, y - hgt + top_gap, slot - g, hgt - top_gap,
                         4 if (j == last and slot >= 8) else 0)
            out.append(f'<path d="{d}" fill="var(--s{sl})" data-tip="{esc(tip)}"/>')
            y -= hgt
        if i % every == 0:
            out.append(f'<text class="tick" x="{x + slot / 2:.1f}" y="{mt + ph + 14}" text-anchor="middle">{a}</text>')
    out.append(f'<line class="axis" x1="{ml}" x2="{ml + pw}" y1="{mt + ph}" y2="{mt + ph}"/>')
    out.append(f'<text class="tick" x="{ml + pw / 2}" y="{height - 2}" text-anchor="middle">rank</text>')
    out.append("</svg>")
    return "".join(out)


def roofline_chart(ceil, points, width=960, height=420):
    """Log-log roofline. ceil: peak, peak_key, all_gflops, dram (the run's
    mix-aware ceiling), dram_read, dram_triad. points: [(label, ai, gflops,
    slot)]."""
    ml, mr, mt, mb = 60, 16, 14, 40
    pw, ph = width - ml - mr, height - mt - mb
    peak = ceil["peak"]
    bw = ceil["dram"]
    ridge = peak / bw
    ais = [p[1] for p in points if p[1] and p[1] > 0] + [ridge]
    gfs = [p[2] for p in points if p[2] and p[2] > 0]
    x0 = 10 ** math.floor(math.log10(min(ais)) - 0.5)
    x1 = 10 ** math.ceil(math.log10(max(ais)) + 0.5)
    y1 = 10 ** math.ceil(math.log10(peak * 1.6))
    y0 = 10 ** math.floor(math.log10(min(gfs + [bw * x0, peak / 100])))
    lx = lambda v: ml + (math.log10(v) - math.log10(x0)) / (math.log10(x1) - math.log10(x0)) * pw
    ly = lambda v: mt + ph - (math.log10(v) - math.log10(y0)) / (math.log10(y1) - math.log10(y0)) * ph
    out = [f'<svg class="chart" viewBox="0 0 {width} {height}" role="img">']
    e = math.floor(math.log10(x0))
    while 10 ** e <= x1 * 1.0001:
        x = lx(10 ** e)
        out.append(f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="{mt}" y2="{mt + ph}"/>'
                   f'<text class="tick" x="{x:.1f}" y="{mt + ph + 14}" text-anchor="middle">{10 ** e:g}</text>')
        e += 1
    e = math.floor(math.log10(y0))
    while 10 ** e <= y1 * 1.0001:
        y = ly(10 ** e)
        out.append(f'<line class="grid" x1="{ml}" x2="{ml + pw}" y1="{y:.1f}" y2="{y:.1f}"/>'
                   f'<text class="tick" x="{ml - 6}" y="{y + 4:.1f}" text-anchor="end">{tick_label(10 ** e)}</text>')
        e += 1
    out.append(f'<text class="tick" x="{ml + pw / 2}" y="{height - 4}" text-anchor="middle">'
               'arithmetic intensity (FLOP per DRAM byte)</text>')
    out.append(f'<text class="tick" transform="translate(14,{mt + ph / 2}) rotate(-90)" text-anchor="middle">GFLOP/s</text>')
    # lower compute ceilings (other widths of the same precision), faint
    prec = ceil["peak_key"].split("_")[0]
    for k, v in sorted((ceil.get("all_gflops") or {}).items(), key=lambda kv: -kv[1]):
        if not k.startswith(prec) or k == ceil["peak_key"] or v <= 0 or v >= peak:
            continue
        xs = max(v / bw, x0)
        out.append(f'<line class="roof2" x1="{lx(xs):.1f}" x2="{ml + pw}" y1="{ly(v):.1f}" y2="{ly(v):.1f}" '
                   f'data-tip="{esc(k)} ceiling: {gflops(v)}"/>'
                   f'<text class="rl2" x="{ml + pw - 4}" y="{ly(v) - 4:.1f}" text-anchor="end">{esc(k)}</text>')
    # the roof: DRAM slope up to the ridge, then the peak
    xa = x0
    ya = bw * xa
    if ya < y0:
        xa = y0 / bw
        ya = y0
    out.append(f'<path class="roof" d="M{lx(xa):.1f},{ly(ya):.1f}L{lx(ridge):.1f},{ly(peak):.1f}H{ml + pw}"/>')
    out.append(f'<line class="hitl" x1="{lx(xa):.1f}" y1="{ly(ya):.1f}" x2="{lx(ridge):.1f}" y2="{ly(peak):.1f}" '
               f'data-tip="DRAM ceiling for this run\'s read mix: {bw:.0f} GB/s"/>')
    out.append(f'<line class="hitl" x1="{lx(ridge):.1f}" y1="{ly(peak):.1f}" x2="{ml + pw}" y2="{ly(peak):.1f}" '
               f'data-tip="{esc(ceil["peak_key"])} peak: {gflops(peak)}"/>')
    out.append(f'<text class="rl" x="{ml + pw - 4}" y="{ly(peak) - 6:.1f}" text-anchor="end">'
               f'{esc(ceil["peak_key"])} peak {gflops(peak)}</text>')
    ang = math.degrees(math.atan2(ly(peak) - ly(ya), lx(ridge) - lx(xa)))
    mx = lx(xa) + (lx(ridge) - lx(xa)) * 0.35
    my = ly(ya) + (ly(peak) - ly(ya)) * 0.35
    out.append(f'<text class="rl" transform="translate({mx:.1f},{my - 6:.1f}) rotate({ang:.1f})">'
               f'DRAM {bw:.0f} GB/s</text>')
    for label, ai, gf, slot in points:
        if not ai or not gf or ai <= 0 or gf <= 0:
            continue
        tip = f"{label}: {gflops(gf)} at {ai:.3g} FLOP/B"
        out.append(f'<circle class="pt" cx="{lx(ai):.1f}" cy="{ly(gf):.1f}" r="5" fill="var(--s{slot})" '
                   f'data-tip="{esc(tip)}"/>')
    placed = []          # direct labels only where they do not collide
    for label, ai, gf, slot in sorted(points, key=lambda p: -(p[2] or 0))[:12]:
        if not (ai and gf and ai > 0 and gf > 0):
            continue
        x, y = lx(ai) + 9, ly(gf) + 4
        if any(abs(y - py) < 14 and abs(x - px) < 120 for px, py in placed):
            continue
        placed.append((x, y))
        out.append(f'<text class="pl" x="{x:.1f}" y="{y:.1f}">{esc(label)}</text>')
    out.append("</svg>")
    return "".join(out)


# ---------------------------------------------------------------- page

CSS = """
.mp{color-scheme:light;--bg:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;
--grid:#e1e0d9;--axis:#c3c2b7;--ring:rgba(11,11,11,.10);
--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;--warn:#fab219;--crit:#d03b3b;--good:#0ca30c}
@media (prefers-color-scheme:dark){:root:where(:not([data-theme="light"])) .mp{color-scheme:dark;
--bg:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500}}
:root[data-theme="dark"] .mp{color-scheme:dark;--bg:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;
--muted:#898781;--grid:#2c2c2a;--axis:#383835;--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500}
*{box-sizing:border-box}
body{margin:0}
.mp{background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh}
.wrap{max-width:1040px;margin:0 auto;padding:28px 20px 60px}
header h1{font-size:20px;font-weight:600;margin:0 0 4px;word-break:break-all}
header .meta{color:var(--ink2);font-size:13px}
nav{display:flex;flex-wrap:wrap;gap:6px 14px;margin:14px 0 4px;font-size:13px}
nav a{color:var(--ink2);text-decoration:none;border-bottom:1px solid var(--axis)}
nav a:hover{color:var(--ink)}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin:18px 0}
.tile{background:var(--surface);border:1px solid var(--ring);border-radius:10px;padding:10px 12px}
.tl{color:var(--ink2);font-size:12px}.tv2{font-size:22px;font-weight:600;margin-top:2px}.ts{color:var(--muted);font-size:12px}
section{background:var(--surface);border:1px solid var(--ring);border-radius:12px;padding:16px 18px;margin:14px 0}
section h2{font-size:16px;font-weight:600;margin:0 0 10px}
section h3{font-size:13px;font-weight:600;margin:16px 0 6px;color:var(--ink2)}
.na{color:var(--ink2)}
.note{color:var(--ink2);font-size:12.5px;margin:6px 0}
.obs{list-style:none;padding:0;margin:0}
.obs li{display:flex;gap:10px;align-items:flex-start;padding:8px 0;border-top:1px solid var(--grid)}
.obs li:first-child{border-top:0}
.obs .ic{flex:none;width:20px;height:20px;border-radius:50%;background:var(--warn);color:#0b0b0b;font-weight:700;
font-size:13px;display:flex;align-items:center;justify-content:center}
.obs .sec{color:var(--muted);font-size:12px;margin-left:6px}
.obs a{color:inherit}
.ok{color:var(--ink2)}
.ok .ic{background:var(--good);color:#fff}
table{border-collapse:collapse;width:100%;font-size:13px;margin:6px 0}
th{font-weight:600;color:var(--ink2);text-align:left;border-bottom:1px solid var(--axis);padding:5px 8px}
td{border-bottom:1px solid var(--grid);padding:5px 8px;vertical-align:top}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
details.tv summary{cursor:pointer;color:var(--ink2);font-size:12.5px;margin-top:4px}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font-size:12.5px;color:var(--ink2);margin:2px 0 6px}
.legend span{display:inline-flex;align-items:center;gap:6px}
.sw{display:inline-block;width:10px;height:10px;border-radius:3px}
.cols{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media (max-width:760px){.cols{grid-template-columns:1fr}}
svg.chart{width:100%;height:auto;display:block;overflow:visible}
svg .grid{stroke:var(--grid);stroke-width:1}
svg .axis{stroke:var(--axis);stroke-width:1}
svg .tick{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}
svg .lab{fill:var(--ink2);font-size:12px}
svg .val{fill:var(--ink2);font-size:11.5px;font-variant-numeric:tabular-nums}
svg .hit{fill:transparent}
svg .hitl{stroke:transparent;stroke-width:12}
svg .roof{fill:none;stroke:var(--ink2);stroke-width:2;stroke-linejoin:round}
svg .roof2{stroke:var(--axis);stroke-width:1.5}
svg .rl{fill:var(--ink2);font-size:11.5px}
svg .rl2{fill:var(--muted);font-size:11px}
svg .pt{stroke:var(--surface);stroke-width:2}
svg .pl{fill:var(--ink);font-size:12px}
svg [data-tip]:hover{opacity:.85}
.inl{display:inline-block;height:8px;border-radius:0 4px 4px 0;background:var(--s1);vertical-align:middle;margin-right:6px}
#tip{position:fixed;pointer-events:none;background:var(--ink);color:var(--surface);font-size:12px;padding:5px 8px;
border-radius:6px;max-width:360px;display:none;z-index:9}
footer{color:var(--muted);font-size:12px;margin-top:24px}
code{font-size:12.5px}
"""

JS = """
(function(){var t=document.getElementById('tip');
document.addEventListener('mousemove',function(e){var el=e.target.closest&&e.target.closest('[data-tip]');
if(!el){t.style.display='none';return;}t.textContent=el.getAttribute('data-tip');t.style.display='block';
var x=e.clientX+14,y=e.clientY+14,w=t.offsetWidth,h=t.offsetHeight;
if(x+w>innerWidth-8)x=e.clientX-w-14;if(y+h>innerHeight-8)y=e.clientY-h-14;t.style.left=x+'px';t.style.top=y+'px';});
})();
"""


def render(doc: dict, obs: list) -> str:
    run = doc.get("run") or {}
    nodes = doc.get("nodes") or []
    m = doc.get("metrics") or {}
    par = m.get("parallel")
    cpu = nodes[0]["cpu"] if nodes else {}
    el = (run.get("elapsed_s") or {}).get("max")
    S = []          # sections, in page order
    nav = []

    def section(key, title, body):
        nav.append((key, title))
        S.append(f'<section id="{key}"><h2>{esc(title)}</h2>{body}</section>')

    def unavailable(blk, what):
        return f'<p class="na">{esc(what)} not measured: {esc((blk or {}).get("reason") or "not collected")}.</p>'

    # ---- tiles
    tiles = [tile("Elapsed", ht(el), "slowest rank")]
    if par:
        sp = par["time_split_pct"]
        tiles.append(tile("MPI", f'{sp["mpi"]:.1f}%', "of elapsed, average rank"))
        tiles.append(tile("Compute imbalance", f'{par["compute"]["imbalance_pct"]:.1f}%',
                          f'costs {par["compute"]["imbalance_cost_pct"]:.1f}% of elapsed'))
        cu = par["cpu"].get("utilisation_per_core_pct") or par["cpu"]["utilisation_pct"]
        tiles.append(tile("CPU utilisation", f'{cu["avg"]:.0f}%', "per physical core"))
    cnt = m.get("counters") or {}
    cd = cnt.get("derived") or {} if cnt.get("available") else {}
    if "gflops_per_s" in cd:
        tiles.append(tile("FLOP rate", gflops(cd["gflops_per_s"]), "whole job"))
    if "ipc" in cd:
        tiles.append(tile("IPC", f'{cd["ipc"]:.2f}', f'{cd.get("ghz_effective", 0):.2f} GHz effective'
                          if cd.get("ghz_effective") else ""))
    mem = m.get("memory") or {}
    if mem.get("available"):
        pn = mem["per_node"]
        tiles.append(tile("DRAM per node", f'{pn["total_gb_s"]["avg"]:.0f} GB/s',
                          f'peak {pn["peak_gb_s_max"]:.0f} GB/s' + (" · partial" if mem.get("coverage") else "")))
        if pn.get("power_w_avg"):
            tiles.append(tile("Package power", f'{pn["power_w_avg"]:.0f} W', "per node"))
    roof = m.get("roofline") or {}
    if roof.get("available") and roof.get("nodes"):
        rn = roof["nodes"][0]
        tiles.append(tile("Roofline", f'{rn["bound"]}-bound', f'{rn["pct_of_attainable"]:.0f}% of attainable'
                          + (" · estimate" if roof.get("estimate") else "")))

    # ---- observations
    if obs:
        items = "".join(f'<li><span class="ic" aria-hidden="true">!</span><span>{esc(o["text"])}'
                        f'<a class="sec" href="#{esc(o["section"])}">{esc(o["section"])} ↓</a></span></li>'
                        for o in obs)
        S.append(f'<section id="observations"><h2>Observations</h2><ul class="obs">{items}</ul></section>')
    elif par:
        S.append('<section id="observations"><h2>Observations</h2><ul class="obs"><li class="ok">'
                 '<span class="ic" aria-hidden="true">✓</span><span>No threshold exceeded.</span></li></ul></section>')

    # ---- time and ranks
    if par:
        sp = par["time_split_pct"]
        body = legend([(1, f'Compute {sp["compute"]:.1f}%'), (2, f'MPI {sp["mpi"]:.1f}%'), (3, f'I/O {sp["io"]:.1f}%')])
        body += hstack([("Average rank", {"c": sp["compute"], "m": sp["mpi"], "i": sp["io"]}, "")],
                       [(1, "c", "Compute"), (2, "m", "MPI"), (3, "i", "I/O")], vmax=100, label_w=110, tail_w=10)
        ranks = doc.get("ranks") or []
        if ranks:
            body += "<h3>Time per rank</h3>" + rank_chart(ranks)
            body += details_table(["rank", "node", "compute", "MPI", "I/O", "elapsed", "CPU util"],
                                  [(r["rank"], r.get("node"), ht(r.get("compute_s")), ht(r.get("mpi_s")),
                                    ht(r.get("io_s")), ht(r.get("elapsed_s")),
                                    num(r.get("cpu_util_per_core_pct") or r.get("cpu_util_pct"), "{:.0f}%"))
                                   for r in sorted(ranks, key=lambda r: r["rank"])])
        comp, mpi = par["compute"], par["mpi"]
        slow = ", ".join(f'{s["rank"]} ({ht(s["compute_s"])})' for s in comp.get("slowest_ranks", [])[:3])
        cu = par["cpu"].get("utilisation_per_core_pct") or par["cpu"]["utilisation_pct"]
        body += "<h3>Balance</h3>" + table(["", "min", "avg", "max", "imbalance"], [
            ("Compute time", ht(comp["time_s"]["min"]), ht(comp["time_s"]["avg"]), ht(comp["time_s"]["max"]),
             f'{comp["imbalance_pct"]:.1f}%'),
            ("MPI time", ht(mpi["time_s"]["min"]), ht(mpi["time_s"]["avg"]), ht(mpi["time_s"]["max"]),
             f'{mpi["imbalance_pct"]:.1f}%'),
            ("CPU utilisation", f'{cu["min"]:.0f}%', f'{cu["avg"]:.0f}%', f'{cu["max"]:.0f}%', "")])
        if slow:
            body += f'<p class="note">Slowest ranks by compute time: {esc(slow)}. Perfect balance would save ' \
                    f'{comp["imbalance_cost_pct"]:.1f}% of the elapsed time.</p>'
        section("parallel", "Time and balance", body)

        # ---- MPI
        funcs = mpi.get("by_function") or []
        if funcs:
            body = "<h3>Top MPI functions, % of elapsed (average rank)</h3>"
            body += ranked_bars([(f["name"], f["pct_of_elapsed"],
                                  f'{f["name"]}: {f["pct_of_elapsed"]:.1f}% of elapsed, {f["calls"]} calls, '
                                  f'imbalance {f["imbalance_pct"]:.0f}%') for f in funcs[:10]])
            body += table(["function", "class", "time %", "imbalance", "calls", "avg size", "data"], text=(1,), rows=
                          [(f["name"], f["class"], f'{f["pct_of_elapsed"]:.1f}%', f'{f["imbalance_pct"]:.1f}%',
                            f'{f["calls"]:,}', hb(f["avg_bytes_per_call"]), hb(f["bytes"])) for f in funcs[:15]])
            ms = mpi.get("message_sizes") or {}
            lowers = ms.get("bin_lower_bytes") or []
            hists = [(c, h) for c, h in ms.items() if c != "bin_lower_bytes" and sum(h.get("calls") or []) > 0]
            if hists and lowers:
                body += "<h3>Message sizes: time spent per size bin</h3>"
                parts = []
                for cls, h in hists:
                    tm = h.get("time_s") or [0] * len(lowers)
                    nz = [i for i, (c, t) in enumerate(zip(h["calls"], tm)) if c or t]
                    a, b = (min(nz), max(nz)) if nz else (0, 0)
                    a, b = max(0, a - 1), min(len(lowers) - 1, b + 1)
                    lab = [hb(lowers[i]).replace(".0 ", " ") for i in range(a, b + 1)]
                    vals = [tm[i] for i in range(a, b + 1)]
                    calls = [h["calls"][i] for i in range(a, b + 1)]
                    tipf = (lambda lab, calls: lambda i, v: f"{lab[i]}+: {ht(v)} in {calls[i]:,} calls")(lab, calls)
                    parts.append(f"<div><div class='note'>{esc(cls)} (seconds, all ranks)</div>"
                                 + columns(vals, lab, width=460, height=170, tipfmt=tipf) + "</div>")
                body += '<div class="cols">' + "".join(parts) + "</div>"
            section("mpi", "MPI", body)

        # ---- I/O
        io = par["io"]
        if any(io[k]["bytes"]["read"] or io[k]["bytes"]["write"] for k in ("posix", "mpiio")):
            rows = []
            for k, lab in (("posix", "POSIX"), ("mpiio", "MPI-IO")):
                x = io[k]
                tt = sum(x["time_s"][q]["avg"] for q in ("read", "write", "meta"))
                rows.append((lab, hb(x["bytes"]["read"]), f'{x["calls"]["read"]:,}', hb(x["bytes"]["write"]),
                             f'{x["calls"]["write"]:,}', ht(tt), ht(max(x["time_s"][q]["max"] for q in ("read", "write", "meta")))))
            section("io", "I/O", table(["", "read", "calls", "written", "calls", "time/rank", "max one rank"], rows))

    # ---- OpenMP
    omp = m.get("openmp")
    if omp is not None:
        if not omp.get("available"):
            section("openmp", "OpenMP", unavailable(omp, "OpenMP"))
        else:
            nr = run.get("nranks") or 1
            tt = omp["thread_time_s"]
            body = f'<p class="note">{esc(omp["runtime"])}, {omp["threads"] / nr:g} threads per rank.</p>'
            body += '<div class="tiles">' + tile("Serial", f'{omp["serial_pct_of_elapsed"]:.1f}%', "outside parallel regions") \
                + tile("Barrier wait", f'{omp["imbalance_pct"]:.1f}%', "of thread time in regions") \
                + tile("Lock wait", f'{omp["lock_wait_pct"]:.1f}%', f'{ht(tt["lock_wait"] / nr)} per rank') + "</div>"
            regs = sorted(omp.get("regions") or [], key=lambda r: -r["wall_s_per_rank"])[:15]
            if regs:
                mx = max(r["imbalance_pct"] for r in regs) or 1
                body += table(["region", "calls/rank", "wall/rank", "team", "barrier wait", "location"], text=(5,), rows=
                              [(r["name"], f'{r["calls"] / max(r["ranks"], 1):g}', ht(r["wall_s_per_rank"]),
                                f'{r["avg_team"]:.1f}',
                                Raw(f'<span class="inl" style="width:{40 * r["imbalance_pct"] / mx:.0f}px"></span>'
                                    f'{r["imbalance_pct"]:.1f}%'), r.get("location", "")) for r in regs])
            section("openmp", "OpenMP", body)

    # ---- counters
    if cnt:
        if not cnt.get("available"):
            section("counters", "Hardware counters", unavailable(cnt, "Hardware counters"))
        else:
            rows = []
            if "ipc" in cd:
                rows.append(("IPC", f'{cd["ipc"]:.2f}', f'outside MPI {cd["ipc_compute"]:.2f}, inside MPI {cd["ipc_mpi"]:.2f}'))
            if cd.get("ghz_effective"):
                rows.append(("Effective clock", f'{cd["ghz_effective"]:.2f} GHz', ""))
            if "gflops_per_s" in cd:
                rows.append(("FLOP rate", gflops(cd["gflops_per_s"]),
                             f'{cd.get("gflops_per_s_per_rank_in_compute", 0):.1f} GFLOP/s per rank while computing'))
            if "flops_per_cycle" in cd:
                rows.append(("FLOPs per cycle", f'{cd["flops_per_cycle"]:.2f}', ""))
            if "l2_misses_per_kilo_instr" in cd:
                rows.append(("L2 misses", f'{cd["l2_misses_per_kilo_instr"]:.2f}', "per 1000 instructions"))
            if "dram_read_gb_per_s" in cd:
                rows.append(("DRAM reads", f'{cd["dram_read_gb_per_s"]:.1f} GB/s',
                             f'this job\'s threads: L1D fills + L2-prefetch misses; '
                             f'{cd.get("flops_per_dram_read_byte", 0):.3g} FLOP per byte read'))
            elif "dram_fill_gb_per_s" in cd:
                rem = cd.get("remote_dram_fill_pct")
                rows.append(("DRAM fills", f'≥ {cd["dram_fill_gb_per_s"]:.1f} GB/s',
                             "L1D fills from DRAM (lower bound)" + (f"; {rem:.1f}% remote" if rem is not None else "")))
            body = f'<p class="note">{esc(cnt["uarch"])}, {esc(cnt["group"])} group, user space, read via ' \
                   f'{esc(cnt["read_method"])}' + (f'; {cnt["multiplexed_ranks"]} rank(s) multiplexed'
                                                   if cnt.get("multiplexed_ranks") else "") + ".</p>"
            body += table(["metric", "value", ""], rows, text=(2,))
            if "fp_ops_width_pct" in cd:
                w = cd["fp_ops_width_pct"]
                body += "<h3>FP-unit ops by vector width</h3>" + legend(
                    [(1, "scalar"), (2, "128-bit"), (3, "256-bit"), (4, "512-bit")])
                body += hstack([("all FP ops", {"a": w["scalar"], "b": w["packed128"], "c": w["packed256"],
                                                "d": w["packed512"]},
                                 f'{w["packed128"] + w["packed256"] + w["packed512"]:.0f}% packed')],
                               [(1, "a", "scalar"), (2, "b", "128-bit"), (3, "c", "256-bit"), (4, "d", "512-bit")],
                               vmax=100, label_w=110)
            regs = cnt.get("regions") or []
            if regs:
                body += "<h3>Instrumented regions</h3>" + table(
                    ["region", "calls", "time/rank", "% of elapsed", "GFLOP/s/rank", "IPC"],
                    [(r["name"], f'{r["calls"]:,}', ht(r["time_s"]["avg"]), num(r.get("pct_of_elapsed"), "{:.1f}%"),
                      num((r.get("derived") or {}).get("gflops_per_s_per_rank"), "{:.1f}"),
                      num((r.get("derived") or {}).get("ipc"), "{:.2f}")) for r in regs[:20]])
            section("counters", "Hardware counters", body)

    # ---- top-down
    td = m.get("topdown")
    if td is not None:
        if not td.get("available"):
            section("topdown", "Top-down", unavailable(td, "Top-down"))
        else:
            keys = [(1, "frontend_bound", "Frontend bound"), (2, "bad_speculation", "Bad speculation"),
                    (3, "backend_bound", "Backend bound"), (4, "retiring", "Retiring")]
            rows = []
            l1 = (td.get("compute") or {}).get("level1") or {}
            if l1:
                rows.append(("whole run", l1, None))
            for r in (td.get("regions") or [])[:15]:
                rl = (r.get("topdown") or {}).get("level1")
                if rl:
                    rows.append((r["name"], rl, None))
            body = f'<p class="note">{esc(td["uarch"])}: % of dispatch slots ({td["dispatch_width"]} per cycle), ' \
                   'outside MPI.' + (f' {esc(td["note"])}' if td.get("note") else "") + "</p>"
            if rows:
                body += legend([(s, n) for s, _, n in keys])
                body += hstack([(n, v, f'retiring {v.get("retiring", 0):.0f}%') for n, v, _ in rows],
                               keys, vmax=100, label_w=150)
                l2 = dict(l1)
                l2.update((td.get("compute") or {}).get("level2") or {})
                t_rows = []
                for r in [("whole run", l2)] + [(r["name"], {**((r.get("topdown") or {}).get("level1") or {}),
                                                             **((r.get("topdown") or {}).get("level2") or {})})
                                                for r in (td.get("regions") or [])[:15]]:
                    n, v = r
                    if not v:
                        continue
                    t_rows.append((n, num(v.get("frontend_bound")), num(v.get("bad_speculation")),
                                   num(v.get("backend_bound")), num(v.get("backend_memory")),
                                   num(v.get("backend_cpu")), num(v.get("retiring"))))
                body += table(["", "frontend", "bad spec.", "backend", "↳ memory", "↳ core", "retiring"], t_rows)
            section("topdown", "Top-down", body)

    # ---- memory
    if m.get("memory") is not None:
        if not mem.get("available"):
            section("memory", "Memory (node)", unavailable(mem, "Node memory traffic"))
        else:
            pn = mem["per_node"]
            body = ""
            if mem.get("coverage"):
                body += f'<p class="note"><b>Partial coverage:</b> {esc(mem["coverage"])}.</p>'
            body += f'<p class="note">Sampled every {mem["interval_ms"]} ms on {mem["nodes_measured"]} node(s)' \
                    + (f'; {mem["nodes_without"]} without: {esc(mem.get("reason_missing"))}' if mem.get("nodes_without") else "") \
                    + f'. Remote reads: {mem["remote_read_share_pct"]:.1f}% of core reads. ' \
                      f'Energy: {mem["energy_j_all_nodes"] / 1e3:.1f} kJ in total.</p>'
            rows = []
            for nd in mem["nodes"]:
                rows.append((nd.get("hostname") or f'node {nd["index"]}', f'{nd["read_gb_s"]:.1f}', f'{nd["write_gb_s"]:.1f}',
                             f'{nd["peak_gb_s"]:.1f}', num(nd.get("pct_of_calibrated_bw"), "{:.0f}%"),
                             num(nd.get("power_w"), "{:.0f} W")))
            body += table(["node", "read GB/s", "write GB/s", "peak GB/s", "of calibrated", "power"], rows)
            n0 = mem["nodes"][0]
            srows = []
            for so in n0["sockets"]:
                ch = so["channels"]
                srows.append((f'socket {so["socket"]}', f'{so["read_gb_s"]:.1f}', f'{so["peak_read_gb_s"]:.1f}',
                              f'{so["write_gb_s"]:.1f}', f'{ch["active"]}/{ch["count"]}',
                              "–" if so["remote_read_share_pct"] is None else f'{so["remote_read_share_pct"]:.0f}%',
                              f'{so["power_w"]:.0f} W'))
            body += f"<h3>{esc(n0.get('hostname') or 'node 0')} by socket</h3>" + table(
                ["", "read GB/s", "peak read", "write GB/s", "channels active", "remote reads", "power"], srows)
            chs = [(so["socket"], i, v) for so in n0["sockets"] for i, v in enumerate(so["channels"].get("read_gb_s") or [])]
            if chs and len(chs) > 1:
                body += f"<h3>Read bandwidth per memory channel ({esc(n0.get('hostname') or 'node 0')})</h3>"
                body += columns([v for _, _, v in chs], [f"s{s}·{i}" for s, i, _ in chs], height=170,
                                unit="", tipfmt=lambda i, v: f"socket {chs[i][0]} channel {chs[i][1]}: {v:.2f} GB/s read")
                body += '<p class="note">Labels: s&lt;socket&gt;·&lt;channel&gt;. Read GB/s, whole run.</p>'
            section("memory", "Memory (node)", body)

    # ---- roofline
    if m.get("roofline") is not None:
        if not roof.get("available"):
            section("roofline", "Roofline", unavailable(roof, "Roofline"))
        else:
            c = roof["ceilings"]
            cal = roof["calibration"]
            nds = roof.get("nodes") or []
            body = ""
            if roof.get("estimate"):
                body += f'<p class="note"><b>Estimate.</b> {esc(roof.get("dram_bytes"))}. Fills miss write-backs ' \
                        'and lines only the L2 prefetchers brought in, so the DRAM bytes are a lower bound and the ' \
                        'points sit to the right of (above) where they belong.</p>'
            body += f'<p class="note">Ceilings from <code>{esc(cal["file"])}</code> ({esc(cal["host"])}, ' \
                   f'{esc(cal["date_utc"][:10])})' + ("" if cal.get("same_cpu") else " — <b>different CPU</b>") \
                   + (f' — <b>{esc(cal["warning"])}</b>' if cal.get("warning") else "") + ".</p>"
            if nds:
                rn = nds[0]
                pts = [(nd.get("hostname") or f'node {nd["index"]}', nd["arithmetic_intensity"], nd["gflops_per_s"], 1)
                       for nd in nds[:8]]
                regs = [g for g in (roof.get("regions") or []) if g.get("arithmetic_intensity_read")]
                regs = sorted(regs, key=lambda g: -g["time_s_per_rank"])[:12]
                pts += [(g["name"], g["arithmetic_intensity_read"], g["gflops_per_s_node"], 2) for g in regs]
                if regs:
                    body += legend([(1, "whole run, per node"), (2, "user regions: FLOP per byte read, rate per node")])
                body += roofline_chart({"peak": c["peak_gflops"], "peak_key": c["peak_key"],
                                        "all_gflops": c.get("all_gflops"), "dram": rn["dram_ceiling_gb_s"]}, pts)
                mix = ("the read-only calibration" if rn.get("read_share_pct") is None
                       else f'node 0\'s read share ({rn["read_share_pct"]:.0f}% reads)')
                body += f'<p class="note">The DRAM roof is for {mix}: ' \
                        f'{c["dram_read_gb_s"]:.0f} GB/s read-only, {c["dram_triad_gb_s"]:.0f} GB/s triad.</p>'
                body += table(["node", "GFLOP/s", "DRAM GB/s", "FLOP/byte", "bound", "of attainable",
                               "of peak FLOPs", "of DRAM bw"],
                              [(nd.get("hostname") or f'node {nd["index"]}', f'{nd["gflops_per_s"]:.1f}',
                                f'{nd["dram_gb_s"]:.1f}', f'{nd["arithmetic_intensity"]:.3g}', nd["bound"],
                                f'{nd["pct_of_attainable"]:.0f}%', f'{nd["pct_of_peak_gflops"]:.1f}%',
                                f'{nd["pct_of_dram_bw"]:.0f}%') for nd in nds])
            regs = [g for g in (roof.get("regions") or []) if g.get("arithmetic_intensity_read")]
            if regs:
                body += "<h3>User regions</h3>" + table(
                    ["region", "time/rank", "GFLOP/s per node", "DRAM read GB/s per node", "FLOP per byte read",
                     "bound", "of attainable"],
                    [(g["name"], ht(g["time_s_per_rank"]), f'{g["gflops_per_s_node"]:.1f}',
                      f'{g["dram_read_gb_s_node"]:.1f}', f'{g["arithmetic_intensity_read"]:.3g}', g["bound"],
                      f'{g["pct_of_attainable"]:.0f}%') for g in sorted(regs, key=lambda g: -g["time_s_per_rank"])],
                    text=(5,))
                body += '<p class="note">Regions: FLOPs per DRAM byte <b>read</b> (L1D fills from DRAM + ' \
                        'L2-prefetcher misses to DRAM, from core counters), against the read-only ceiling; writes ' \
                        'are not counted, so a region that writes a lot sits further right than its true intensity. ' \
                        'Counts are those of the threads that entered the region.</p>'
            elif roof.get("regions_reason"):
                body += f'<p class="note">Per-region points: {esc(roof["regions_reason"])}.</p>'
            section("roofline", "Roofline", body)

    # ---- run details
    rows = [("Command", run.get("command")), ("Ranks", f'{run.get("nranks")} on {run.get("nnodes")} node(s)'),
            ("Threads per rank", run.get("omp_num_threads") or "–"), ("MPI", run.get("mpi_library")),
            ("MPI thread level", run.get("mpi_thread_level")), ("Slurm job", run.get("slurm_job_id") or "–"),
            ("Started (UTC)", run.get("start_utc")), ("Snapshot level", run.get("level"))]
    ov = doc.get("overhead") or {}
    if ov:
        rows.append(("miniprof overhead", f'{100 * (ov.get("fraction_of_elapsed") or 0):.2f}% of elapsed'))
    nrows = [(n.get("hostname"), n["cpu"].get("brand"), n["cpu"].get("uarch_name") or n["cpu"].get("uarch"),
              n.get("ranks"), n.get("logical_cpus"), n.get("numa_nodes"), n.get("kernel")) for n in nodes[:64]]
    body = table(["", ""], rows, text=(1,)) + "<h3>Nodes</h3>" + table(
        ["host", "CPU", "microarchitecture", "ranks", "logical CPUs", "NUMA nodes", "kernel"], nrows, text=(1, 2, 6))
    section("run", "Run", body)

    title = f'miniprof — {run.get("command") or "snapshot"}'
    meta = f'{run.get("nranks")} ranks on {run.get("nnodes")} node(s) · ' \
           f'{cpu.get("uarch_name") or cpu.get("brand") or "unknown CPU"} · {run.get("start_utc") or ""}'
    navh = "".join(f'<a href="#{k}">{esc(t)}</a>' for k, t in nav)
    tool = doc.get("tool") or {}
    return ("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            f"<title>{esc(title)}</title><style>{CSS}</style></head><body><div class=\"mp\"><div class=\"wrap\">"
            f"<header><h1>{esc(run.get('command') or 'miniprof snapshot')}</h1><div class=\"meta\">{esc(meta)}</div>"
            f"<nav>{navh}</nav></header>"
            f'<div class="tiles">{"".join(tiles)}</div>'
            + "".join(S) +
            f'<footer>{esc(tool.get("name", "miniprof"))} {esc(tool.get("version", ""))} · snapshot schema '
            f'{esc(doc.get("schema_version", "?"))} · generated by miniprof_report.py --html</footer>'
            f'</div><div id="tip" role="tooltip"></div></div><script>{JS}</script></body></html>\n')
