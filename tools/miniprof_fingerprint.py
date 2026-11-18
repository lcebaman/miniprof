#!/usr/bin/env python3
"""
miniprof_fingerprint.py — characterise a run, and compare runs, from snapshots.

A fingerprint is a fixed set of dimensionless axes, each a percentage, plus a
few absolute descriptors and a one-line class, all derived from snapshot.json
after the run (no extra cost while the application runs). Axes the snapshot
level did not measure are null, never guessed: 'basic' gives the time axes,
'topdown' adds the core axes, 'roofline'/'full' the machine axes.

    time     share of elapsed (average rank) spent in MPI, I/O, waiting for the
             slowest rank (imbalance), serial OpenMP code, OpenMP barriers
    core     top-down share of the compute pipeline slots: retiring, frontend,
             bad speculation, backend memory, backend core
    machine  share of what the node can do: CPU utilisation, DRAM bandwidth
             (of calibrated), FLOP rate (of calibrated peak), packed FP ops

The class names the largest share of the run and, for compute, what limits
it: e.g. "compute 71% memory-bandwidth bound; MPI 18% latency (small messages)".

Usage:
    miniprof_fingerprint.py RUN [RUN ...] [--names a,b,..] [--json OUT]
                            [--csv OUT] [--html OUT]
RUN is a snapshot JSON or a miniprof result folder (containing snapshot.json).
One RUN prints its fingerprint; several print them side by side.
"""

import argparse
import json
import os
import sys

# Rules for the compute class (percent). Kept here, not in the thresholds
# file of miniprof_report: the class is a description, not an alarm.
DRAM_BW_BOUND = 60.0       # >= this % of calibrated DRAM bandwidth: bandwidth bound
DRAM_BW_LOW = 40.0         # backend-memory stalls with < this % bandwidth: latency bound
FLOP_BOUND = 50.0          # >= this % of calibrated peak FLOPs: FLOP bound
TD_STALL = 25.0            # top-down category this large names the limiter
TD_BADSPEC = 15.0
TD_RETIRING = 50.0
SMALL_MSG = 16384          # time-weighted median p2p message below this: latency (small messages)

AXES = [
    # key, group, label, meaning
    ("mpi", "time", "MPI", "share of elapsed inside MPI calls, including waiting for slower ranks"),
    ("imbalance", "time", "Rank imbalance", "share of elapsed the average rank waits (in MPI) for the slowest: (max - avg) compute"),
    ("io", "time", "I/O", "share of elapsed in POSIX and MPI-IO calls"),
    ("omp_serial", "time", "OpenMP serial", "share of elapsed computing outside parallel regions"),
    ("omp_wait", "time", "OpenMP barrier wait", "share of thread time waiting at barriers"),
    ("retiring", "core", "Retiring", "pipeline slots doing useful work"),
    ("frontend", "core", "Frontend bound", "slots empty: instruction fetch/decode"),
    ("bad_spec", "core", "Bad speculation", "slots wasted on squashed ops"),
    ("backend_memory", "core", "Backend memory", "slots stalled waiting for data"),
    ("backend_core", "core", "Backend core", "slots stalled on execution units / dependencies"),
    ("cpu_util", "machine", "CPU utilisation", "CPU time over the cores the ranks may use"),
    ("dram_bw", "machine", "DRAM bandwidth", "node DRAM traffic, % of calibrated bandwidth"),
    ("flops_peak", "machine", "FLOP rate", "% of calibrated peak FLOP rate"),
    ("vector", "machine", "Packed FP ops", "FP-unit ops that are packed (vector)"),
]
GROUPS = [("time", "Where the time goes (% of elapsed)"),
          ("core", "Compute pipeline, top-down (% of slots)"),
          ("machine", "Use of the node (% of capability)")]
DESCRIPTORS = [
    # key, label, format
    ("ipc", "IPC", "{:.2f}"),
    ("ghz", "Effective GHz", "{:.2f}"),
    ("gflops_node", "GFLOP/s per node", "{:.0f}"),
    ("dram_gb_s_node", "DRAM GB/s per node", "{:.0f}"),
    ("arith_intensity", "FLOP per DRAM byte", "{:.2f}"),
    ("l2_mpki", "L2 misses / kilo-instr", "{:.1f}"),
    ("remote_read_pct", "Remote DRAM reads %", "{:.1f}"),
    ("mpi_mb_s_rank", "MPI MB/s per rank", "{:.1f}"),
    ("mpi_msg_bytes", "Typical p2p message (by time)", None),
    ("mpi_collective_pct", "Collective share of MPI %", "{:.0f}"),
    ("rss_mb_rank", "Max RSS MB per rank", "{:.0f}"),
    ("power_w_node", "Package W per node", "{:.0f}"),
]


def _avg(x):
    return x.get("avg") if isinstance(x, dict) else x


def _num(x):
    return x if isinstance(x, (int, float)) and not isinstance(x, bool) else None


def human_bytes(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.0f} {unit}"
        n /= 1024.0


def load(path):
    p = os.path.join(path, "snapshot.json") if os.path.isdir(path) else path
    with open(p) as f:
        return json.load(f)


def _msg_profile(mpi):
    """Time-weighted median point-to-point message size (bin lower bound)
    and the collective share of MPI time."""
    ms = mpi.get("message_sizes") or {}
    lower = ms.get("bin_lower_bytes") or []
    t = [0.0] * len(lower)
    for i, v in enumerate((ms.get("p2p") or {}).get("time_s") or []):
        if i < len(t):
            t[i] += v or 0
    median = None
    tot = sum(t)
    if tot > 0:
        acc = 0.0
        for i, v in enumerate(t):
            acc += v
            if acc >= tot / 2:
                median = lower[i]
                break
    bc = mpi.get("by_class") or {}
    pct = {k: (bc.get(k) or {}).get("pct_of_elapsed") or 0 for k in ("p2p", "collective", "wait", "rma")}
    s = sum(pct.values())
    coll = 100.0 * pct["collective"] / s if s > 0 else None
    return median, coll


def fingerprint(doc):
    """{"axes": {key: pct|None}, "descriptors": {...}, "class": str,
    "components": [(name, pct, detail)], "level": str}"""
    from miniprof_report import _sections, serial_compute_pct
    m = doc.get("metrics") or {}
    par = m.get("parallel") or {}
    run = doc.get("run") or {}
    ax = {k: None for k, *_ in AXES}
    ds = {k: None for k, *_ in DESCRIPTORS}
    out = {"axes": ax, "descriptors": ds, "level": run.get("level"), "class": None, "components": []}
    if not par:
        out["class"] = "no MPI data"
        return out
    v = _sections(doc)
    split, comp, tdc, cd, rn = v["split"], v["comp"], v["tdc"], v["cd"], v["rn"]
    omp = v["omp"] if (v["omp"] or {}).get("available") else None
    mem = m.get("memory") or {}
    mem = mem if mem.get("available") else None

    # ---- time axes
    ax["mpi"] = _num(split.get("mpi"))
    ax["io"] = _num(split.get("io"))
    ax["imbalance"] = _num(comp.get("imbalance_cost_pct"))
    if omp:
        ax["omp_serial"] = serial_compute_pct(omp, split)
        ax["omp_wait"] = _num(omp.get("imbalance_pct"))
    # ---- core axes
    if tdc:
        for k, src in (("retiring", "retiring"), ("frontend", "frontend_bound"),
                       ("bad_spec", "bad_speculation"), ("backend_memory", "backend_memory"),
                       ("backend_core", "backend_cpu")):
            ax[k] = _num(tdc.get(src))
    # ---- machine axes
    ax["cpu_util"] = _num(_avg(v["cpuu"]))
    if rn and not v["roof_est"]:
        ax["dram_bw"] = _num(rn.get("pct_of_dram_bw"))
        ax["flops_peak"] = _num(rn.get("pct_of_peak_gflops"))
    if ax["dram_bw"] is None and mem:
        socks = [s.get("pct_of_calibrated_bw") for n in mem.get("nodes") or [] for s in n.get("sockets") or []]
        socks = [s for s in socks if _num(s) is not None]
        if socks:
            ax["dram_bw"] = sum(socks) / len(socks)
    ax["vector"] = _num(cd.get("packed_fp_ops_pct"))
    for k in ax:
        if ax[k] is not None:
            ax[k] = max(0.0, min(100.0, float(ax[k])))

    # ---- descriptors
    nnodes = run.get("nnodes") or 1
    nranks = run.get("nranks") or 1
    el = _avg(run.get("elapsed_s") or {}) or 0
    ds["ipc"] = _num(cd.get("ipc_compute", cd.get("ipc")))
    ds["ghz"] = _num(cd.get("ghz_effective"))
    if _num(cd.get("gflops_per_s")) is not None:
        ds["gflops_node"] = cd["gflops_per_s"] / nnodes
    if mem and mem.get("per_node"):
        ds["dram_gb_s_node"] = _num(_avg(mem["per_node"].get("total_gb_s")))
        ds["power_w_node"] = _num(mem["per_node"].get("power_w_avg"))
        ds["remote_read_pct"] = _num(mem.get("remote_read_share_pct"))
    if rn:
        ds["arith_intensity"] = _num(rn.get("arithmetic_intensity"))
        if ds["dram_gb_s_node"] is None:
            ds["dram_gb_s_node"] = _num(rn.get("dram_gb_s"))
    elif _num(cd.get("flops_per_dram_fill_byte")) is not None:
        ds["arith_intensity"] = cd["flops_per_dram_fill_byte"]
    ds["l2_mpki"] = _num(cd.get("l2_misses_per_kilo_instr"))
    if ds["remote_read_pct"] is None:
        ds["remote_read_pct"] = _num(cd.get("remote_dram_fill_pct", cd.get("remote_dram_read_pct")))
    mpi = par.get("mpi") or {}
    tot_bytes = sum((c or {}).get("bytes") or 0 for c in (mpi.get("by_class") or {}).values())
    if el > 0 and tot_bytes:
        ds["mpi_mb_s_rank"] = tot_bytes / nranks / el / 1e6
    ds["mpi_msg_bytes"], ds["mpi_collective_pct"] = _msg_profile(mpi)
    ds["rss_mb_rank"] = _num(_avg((par.get("cpu") or {}).get("max_rss_mb") or {}))

    # ---- class: split the run into components, name the compute limiter
    # The average rank spends the imbalance cost waiting inside MPI for the
    # slowest rank, so it is carved out of MPI time, not compute.
    comps = []
    imb = min(ax["imbalance"] or 0, ax["mpi"] or 0)
    mpi_pct = (ax["mpi"] or 0) - imb
    if mpi_pct > 0:
        if ds["mpi_collective_pct"] is not None and ds["mpi_collective_pct"] >= 60:
            det = "collectives"
        elif ds["mpi_msg_bytes"] is not None:
            det = ("latency (small messages)" if ds["mpi_msg_bytes"] < SMALL_MSG
                   else "bandwidth (large messages)")
        else:
            det = ""
        comps.append(("MPI", mpi_pct, det))
    if ax["io"]:
        comps.append(("I/O", ax["io"], ""))
    if imb:
        comps.append(("rank imbalance", imb, ""))
    serial = ax["omp_serial"] or 0
    if serial:
        comps.append(("OpenMP serial", serial, ""))
    ompw = 0.0
    if omp and ax["omp_wait"] and omp.get("parallel_pct_of_elapsed") is not None:
        ompw = ax["omp_wait"] * omp["parallel_pct_of_elapsed"] / 100.0
        comps.append(("OpenMP barrier wait", ompw, ""))
    compute = max(0.0, (split.get("compute") or 0) - serial - ompw)
    why = None
    if not (m.get("counters") or {}).get("available"):
        why = "no hardware counters"
    elif run.get("level") == "basic":
        why = "needs -l topdown or higher"
    comps.append(("compute", compute, compute_class(ax, rn, tdc, why)))
    comps.sort(key=lambda c: -c[1])
    out["components"] = [{"name": n, "pct": p, "detail": d} for n, p, d in comps]
    shown = [c for c in comps if c[1] >= 5] or comps[:1]
    out["class"] = "; ".join(f"{n} {p:.0f}%" + (f" {d}" if d else "") for n, p, d in shown[:3])
    return out


def compute_class(ax, rn, tdc, why=None):
    """What limits the compute part, from the strongest evidence available."""
    bw, fl = ax["dram_bw"], ax["flops_peak"]
    if bw is not None and bw >= DRAM_BW_BOUND:
        return "memory-bandwidth bound"
    if fl is not None and fl >= FLOP_BOUND:
        return "FLOP bound"
    if tdc:
        cand = [("backend_memory", ax["backend_memory"]), ("backend_core", ax["backend_core"]),
                ("frontend", ax["frontend"]), ("bad_spec", ax["bad_spec"])]
        cand = [(k, x) for k, x in cand if x is not None]
        k, x = max(cand, key=lambda c: c[1]) if cand else (None, 0)
        if k == "backend_memory" and x >= TD_STALL:
            if bw is None:
                return "memory bound"
            return "memory-latency bound" if bw < DRAM_BW_LOW else "memory bound"
        if k == "backend_core" and x >= TD_STALL:
            return "core bound (execution units, dependencies)"
        if k == "frontend" and x >= TD_STALL:
            return "frontend bound"
        if k == "bad_spec" and x >= TD_BADSPEC:
            return "bad-speculation bound"
        if (ax["retiring"] or 0) >= TD_RETIRING:
            return "retiring well"
        return "mixed limits"
    if rn:
        return ("memory side of the roofline" if rn.get("bound") == "memory"
                else "compute side of the roofline")
    return f"not characterised ({why or 'needs -l topdown or higher'})"


def fmt_desc(key, val):
    if val is None:
        return "–"
    if key == "mpi_msg_bytes":
        return human_bytes(val) + "+"
    f = dict((k, f) for k, _, f in DESCRIPTORS)[key]
    return f.format(val)


def run_label(doc, path):
    if os.path.isdir(path):
        return os.path.basename(os.path.normpath(path))
    cmd = (doc.get("run") or {}).get("command") or path
    return os.path.basename(cmd.split()[0]) if cmd.split() else path


# ---------------------------------------------------------------- outputs

def text_one(fp, width=78):
    out = [f" Fingerprint            {fp['class']}"]
    for g, title in GROUPS:
        rows = [(lab, fp["axes"][k]) for k, gg, lab, _ in AXES if gg == g]
        if all(x is None for _, x in rows):
            continue
        out.append(f"   {title}")
        for lab, x in rows:
            if x is None:
                continue
            n = int(round(x / 100 * 30))
            out.append(f"     {lab:<22} {x:5.1f}%  " + "#" * n + "." * (30 - n))
    return out


def text_many(fps, names):
    w = max(10, max(len(n) for n in names) + 1)
    lw = 26
    out = [" " * lw + "".join(f"{n[:w - 1]:>{w}}" for n in names)]
    for g, title in GROUPS:
        out.append(title)
        for k, gg, lab, _ in AXES:
            if gg != g:
                continue
            vals = [f["axes"][k] for f in fps]
            if all(x is None for x in vals):
                continue
            out.append(f"  {lab:<{lw - 2}}" + "".join(f"{('–' if x is None else f'{x:.1f}'):>{w}}" for x in vals))
    out.append("Descriptors")
    for k, lab, _ in DESCRIPTORS:
        vals = [f["descriptors"][k] for f in fps]
        if all(x is None for x in vals):
            continue
        out.append(f"  {lab:<{lw - 2}}" + "".join(f"{fmt_desc(k, x):>{w}}" for x in vals))
    out.append("Class")
    for n, f in zip(names, fps):
        out.append(f"  {n}: {f['class']}")
    return out


def html_section(fps, names):
    """Body of a fingerprint section: one table, a column per run, each
    value drawn as an inline bar on a fixed 0-100 scale."""
    import miniprof_html as H

    def cell(x):
        if x is None:
            return H.Raw('<span class="na">–</span>')
        return H.Raw(f'<span class="fpt" title="{x:.1f}%"><i style="width:{max(1.0, x):.1f}%"></i></span>{x:.1f}')

    body = []
    if len(fps) == 1:
        body.append(f'<p class="fpclass">{H.esc(fps[0]["class"])}</p>')
    else:
        body.append("<ul class=\"fpclasses\">" + "".join(
            f"<li><b>{H.esc(n)}</b> — {H.esc(f['class'])}</li>" for n, f in zip(names, fps)) + "</ul>")
    head = [""] + (names if len(fps) > 1 else ["%"])
    for g, title in GROUPS:
        rows = []
        for k, gg, lab, meaning in AXES:
            if gg != g:
                continue
            vals = [f["axes"][k] for f in fps]
            if all(x is None for x in vals):
                continue
            rows.append([H.Raw(f'<span title="{H.esc(meaning)}">{H.esc(lab)}</span>')] + [cell(x) for x in vals])
        if rows:
            body.append(f"<h3>{H.esc(title)}</h3>" + H.table(head, rows, cls="fp"))
    rows = []
    for k, lab, _ in DESCRIPTORS:
        vals = [f["descriptors"][k] for f in fps]
        if all(x is None for x in vals):
            continue
        rows.append([lab] + [fmt_desc(k, x) for x in vals])
    if rows:
        body.append("<h3>Descriptors</h3>" + H.table(head if len(fps) > 1 else ["", ""], rows, cls="fp"))
    gaps = []
    if any(f["axes"]["retiring"] is None for f in fps):
        gaps.append("the top-down axes need <code>-l topdown</code> or higher and hardware counters")
    if any(f["axes"]["dram_bw"] is None or f["axes"]["flops_peak"] is None for f in fps):
        gaps.append("DRAM bandwidth and FLOP rate need <code>-l roofline</code> or <code>full</code>, "
                    "counters and a calibration")
    if gaps:
        body.append('<p class="note">– = not measured: ' + "; ".join(gaps) + ".</p>")
    return "".join(body)


def html_page(fps, names, docs):
    import miniprof_html as H
    title = "miniprof fingerprints — " + ", ".join(names)
    body = html_section(fps, names)
    meta = " · ".join(f'{n}: {(d.get("run") or {}).get("nranks")} ranks, '
                      f'{((d.get("nodes") or [{}])[0].get("cpu") or {}).get("uarch_name") or "?"}, '
                      f'level {(d.get("run") or {}).get("level")}' for n, d in zip(names, docs))
    return ("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            f"<title>{H.esc(title)}</title><style>{H.CSS}</style></head><body><div class=\"mp\"><div class=\"wrap\">"
            f"<header><h1>{H.esc(title)}</h1><div class=\"meta\">{H.esc(meta)}</div></header>"
            f'<section id="fingerprint"><h2>Fingerprint</h2>{body}</section>'
            "<footer>generated by miniprof_fingerprint.py</footer>"
            f'</div><div id="tip" role="tooltip"></div></div><script>{H.JS}</script></body></html>\n')


def to_csv(fps, names):
    lines = ["key,group,label," + ",".join(names)]
    for k, g, lab, _ in AXES:
        lines.append(f"{k},{g},{lab}," + ",".join("" if f["axes"][k] is None else f"{f['axes'][k]:.2f}" for f in fps))
    for k, lab, _ in DESCRIPTORS:
        lines.append(f"{k},descriptor,{lab}," + ",".join(
            "" if f["descriptors"][k] is None else f"{f['descriptors'][k]:.4g}" for f in fps))
    lines.append("class,class,Class," + ",".join('"' + f["class"].replace('"', "'") + '"' for f in fps))
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Characterise miniprof runs and compare them side by side.")
    ap.add_argument("runs", nargs="+", help="snapshot JSON files or miniprof result folders")
    ap.add_argument("--names", help="comma-separated column names (default: folder or program name)")
    ap.add_argument("--json", metavar="OUT", help="write the fingerprints as JSON")
    ap.add_argument("--csv", metavar="OUT", help="write a CSV (rows = axes, columns = runs)")
    ap.add_argument("--html", metavar="OUT", help="write an HTML comparison page")
    a = ap.parse_args(argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    docs, fps = [], []
    for p in a.runs:
        try:
            d = load(p)
        except (OSError, ValueError) as e:
            print(f"miniprof-fingerprint: {p}: {e}", file=sys.stderr)
            return 1
        docs.append(d)
        fps.append(fingerprint(d))
    names = a.names.split(",") if a.names else [run_label(d, p) for d, p in zip(docs, a.runs)]
    if len(names) != len(fps):
        print("miniprof-fingerprint: --names needs one name per run", file=sys.stderr)
        return 1
    seen = {}
    for i, n in enumerate(names):          # make duplicate labels distinct
        if n in seen:
            names[i] = f"{n}#{i + 1}"
        seen[n] = 1
    print("\n".join(text_one(fps[0]) if len(fps) == 1 else text_many(fps, names)))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"schema": "miniprof-fingerprint", "schema_version": "1.0",
                       "runs": [dict(name=n, source=p, **fp) for n, p, fp in zip(names, a.runs, fps)]}, f, indent=1)
    if a.csv:
        with open(a.csv, "w") as f:
            f.write(to_csv(fps, names))
    if a.html:
        with open(a.html, "w") as f:
            f.write(html_page(fps, names, docs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
