#!/usr/bin/env python3
"""
miniprof_report.py — one-page text summary of a miniprof snapshot JSON.

APS-style: where the time went (compute / MPI / I/O), how unevenly, which
MPI calls and message sizes dominate, I/O volume, CPU utilisation, and a
short list of observations from simple thresholds. Version 0: the
threshold rules here are placeholders for the guidance engine of phase P7.

Usage:
    miniprof_report.py SNAPSHOT.json [-o OUT.txt]
                       [--mpi-pct 20] [--imbalance-pct 10]
                       [--io-pct 10] [--cpu-util-pct 80]
"""

import argparse
import json
import sys

W = 78


def human_bytes(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} TiB"


def human_time(s: float) -> str:
    if s >= 100:
        return f"{s:.0f} s"
    if s >= 1:
        return f"{s:.2f} s"
    if s >= 1e-3:
        return f"{s * 1e3:.1f} ms"
    return f"{s * 1e6:.0f} us"


def bar(pct: float, width: int = 30) -> str:
    n = int(round(max(0.0, min(100.0, pct)) / 100.0 * width))
    return "#" * n + "." * (width - n)


def bin_label(lower: int) -> str:
    if lower == 0:
        return "0 B"
    return f"{human_bytes(lower)}-{human_bytes(lower * 2)}"


def report(doc: dict, args) -> str:
    out = []
    p = out.append

    run = doc.get("run", {})
    nodes = doc.get("nodes", [])
    par = (doc.get("metrics") or {}).get("parallel")

    cpu = nodes[0]["cpu"] if nodes else {}
    cpu_desc = cpu.get("uarch_name") or cpu.get("brand") or "unknown CPU"
    p("=" * W)
    p(f" miniprof snapshot  |  {run.get('command') or '?'}")
    p(f" {run.get('nranks')} ranks on {run.get('nnodes')} node(s)  |  {cpu_desc}"
      f"  |  {run.get('start_utc') or ''}")
    p("=" * W)
    el = run.get("elapsed_s", {})
    p(f" Elapsed time          {human_time(el.get('max', 0))}  (slowest rank)")

    if not par:
        p("")
        p(" No parallel metrics in this snapshot (collected before phase P1).")
        return "\n".join(out) + "\n"

    split = par["time_split_pct"]
    p("")
    p(" Time split (average over ranks)")
    for key, label in (("compute", "Compute"), ("mpi", "MPI"), ("io", "I/O")):
        p(f"   {label:<10} {split[key]:6.1f}%  {bar(split[key])}")

    comp = par["compute"]
    mpi = par["mpi"]
    io = par["io"]
    cpuu = par["cpu"]["utilisation_pct"]
    p("")
    p(f" Compute imbalance     {comp['imbalance_pct']:5.1f}%   "
      f"(perfect balance would save {comp['imbalance_cost_pct']:.1f}% of elapsed)")
    slow = ", ".join(f"{s['rank']} ({human_time(s['compute_s'])})"
                     for s in comp.get("slowest_ranks", [])[:3])
    if slow:
        p(f"   slowest ranks       {slow}")
    p(f" MPI time imbalance    {mpi['imbalance_pct']:5.1f}%   "
      f"(min {human_time(mpi['time_s']['min'])}, max {human_time(mpi['time_s']['max'])})")
    p(f" CPU utilisation       {cpuu['avg']:5.1f}%   (min {cpuu['min']:.1f}%, max {cpuu['max']:.1f}%)")

    # MPI detail
    funcs = mpi.get("by_function", [])
    if funcs:
        p("")
        p(" MPI by class           time%    calls        data")
        for cls, c in mpi["by_class"].items():
            if c["calls"]:
                p(f"   {cls:<18} {c['pct_of_elapsed']:6.1f}%  {c['calls']:>8}  {human_bytes(c['bytes']):>10}")
        p("")
        p(" Top MPI functions      time%    imb%    calls   avg size")
        for f in funcs[:8]:
            p(f"   {f['name']:<18} {f['pct_of_elapsed']:6.1f}%  {f['imbalance_pct']:5.1f}%  "
              f"{f['calls']:>7}  {human_bytes(f['avg_bytes_per_call']):>9}")

        ms = mpi.get("message_sizes", {})
        lowers = ms.get("bin_lower_bytes", [])
        rows = []
        for cls, h in ms.items():
            if cls == "bin_lower_bytes":
                continue
            calls = h["calls"]
            total = sum(calls)
            if not total:
                continue
            b = max(range(len(calls)), key=lambda i: calls[i])
            t = h["time_s"]
            bt = max(range(len(t)), key=lambda i: t[i])
            rows.append(f"   {cls:<12} most calls: {bin_label(lowers[b])} ({100 * calls[b] / total:.0f}%)"
                        f";  most time: {bin_label(lowers[bt])}")
        if rows:
            p("")
            p(" Message sizes")
            out.extend(rows)

    # I/O
    p("")
    p(" I/O                    read                 write             time (avg/rank)")
    for kind, label in (("posix", "POSIX"), ("mpiio", "MPI-IO")):
        k = io[kind]
        tr, tw, tm = (k["time_s"][x]["avg"] for x in ("read", "write", "meta"))
        p(f"   {label:<8} {human_bytes(k['bytes']['read']):>10} ({k['calls']['read']:>6} calls)"
          f" {human_bytes(k['bytes']['write']):>10} ({k['calls']['write']:>6} calls)"
          f"   {human_time(tr + tw + tm)}")

    # observations
    obs = []
    if split["mpi"] > args.mpi_pct:
        top = funcs[0]["name"] if funcs else "MPI"
        obs.append(f"MPI takes {split['mpi']:.1f}% of the run (> {args.mpi_pct:g}%); "
                   f"largest contributor {top}.")
    if comp["imbalance_pct"] > args.imbalance_pct:
        obs.append(f"Compute is imbalanced across ranks ({comp['imbalance_pct']:.1f}% > "
                   f"{args.imbalance_pct:g}%); much of the MPI wait time is likely ranks "
                   "waiting for the slowest ones.")
    if split["io"] > args.io_pct:
        obs.append(f"I/O takes {split['io']:.1f}% of the run (> {args.io_pct:g}%).")
    if cpuu["avg"] < args.cpu_util_pct:
        obs.append(f"CPU utilisation is {cpuu['avg']:.1f}% (< {args.cpu_util_pct:g}%): "
                   "ranks may be bound to more CPUs than they use, or be idle/blocked.")
    p("")
    p(" Observations")
    if obs:
        for o in obs:
            p(f"   ! {o}")
    else:
        p("   No threshold exceeded.")
    p("=" * W)
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("file")
    ap.add_argument("-o", "--output")
    ap.add_argument("--mpi-pct", type=float, default=20.0)
    ap.add_argument("--imbalance-pct", type=float, default=10.0)
    ap.add_argument("--io-pct", type=float, default=10.0)
    ap.add_argument("--cpu-util-pct", type=float, default=80.0)
    args = ap.parse_args()
    with open(args.file) as f:
        doc = json.load(f)
    text = report(doc, args)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
