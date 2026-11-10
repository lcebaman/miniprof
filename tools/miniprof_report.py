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
                       [--remote-pct 20] [--packed-pct 50]
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
    cpuu = par["cpu"].get("utilisation_per_core_pct") or par["cpu"]["utilisation_pct"]
    p("")
    p(f" Compute imbalance     {comp['imbalance_pct']:5.1f}%   "
      f"(perfect balance would save {comp['imbalance_cost_pct']:.1f}% of elapsed)")
    slow = ", ".join(f"{s['rank']} ({human_time(s['compute_s'])})"
                     for s in comp.get("slowest_ranks", [])[:3])
    if slow:
        p(f"   slowest ranks       {slow}")
    p(f" MPI time imbalance    {mpi['imbalance_pct']:5.1f}%   "
      f"(min {human_time(mpi['time_s']['min'])}, max {human_time(mpi['time_s']['max'])})")
    p(f" CPU utilisation       {cpuu['avg']:5.1f}%   (min {cpuu['min']:.1f}%, max {cpuu['max']:.1f}%; "
      "per physical core)")

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

    # hardware counters
    cnt = (doc.get("metrics") or {}).get("counters")
    cd = {}
    if cnt:
        p("")
        if not cnt.get("available"):
            p(f" Hardware counters      unavailable: {cnt.get('reason')}")
        else:
            cd = cnt.get("derived", {})
            p(f" Hardware counters      {cnt['uarch']}, {cnt['group']} group, "
              f"user space, reads via {cnt['read_method']}"
              + (f", {cnt['multiplexed_ranks']} rank(s) MULTIPLEXED" if cnt.get("multiplexed_ranks") else ""))
            if "ipc" in cd:
                p(f"   IPC                 {cd['ipc']:.2f}  (outside MPI {cd['ipc_compute']:.2f}, "
                  f"inside MPI {cd['ipc_mpi']:.2f})")
            if cd.get("ghz_effective"):
                p(f"   Effective clock     {cd['ghz_effective']:.2f} GHz")
            if "gflops_per_s" in cd:
                line = f"   FLOP rate           {cd['gflops_per_s']:.2f} GFLOP/s total"
                if "gflops_per_s_per_rank_in_compute" in cd:
                    line += f", {cd['gflops_per_s_per_rank_in_compute']:.2f} per rank while computing"
                p(line)
                if "flops_per_cycle" in cd:
                    p(f"   FLOPs per cycle     {cd['flops_per_cycle']:.2f}")
            if "l2_misses_per_kilo_instr" in cd:
                p(f"   L2 misses           {cd['l2_misses_per_kilo_instr']:.2f} per 1000 instructions")
            if "dram_fill_gb_per_s" in cd:
                rem = cd.get("remote_dram_fill_pct")
                p(f"   DRAM fills          >= {cd['dram_fill_gb_per_s']:.2f} GB/s (L1D fills from DRAM, "
                  f"lower bound)" + (f"; {rem:.1f}% from a remote NUMA node" if rem is not None else ""))
            if "fp_ops_width_pct" in cd:
                w = cd["fp_ops_width_pct"]
                p(f"   FP-unit ops/width   scalar {w['scalar']:.1f}% | 128-bit {w['packed128']:.1f}% | "
                  f"256-bit {w['packed256']:.1f}% | 512-bit {w['packed512']:.1f}%")
        regs = cnt.get("regions") or []
        if regs:
            p("")
            p(" Regions                calls   time/rank   GFLOP/s/rank    IPC")
            for r in regs[:12]:
                dv = r.get("derived", {})
                gf = f"{dv['gflops_per_s_per_rank']:12.2f}" if "gflops_per_s_per_rank" in dv else " " * 12
                ipc = f"{dv['ipc']:7.2f}" if "ipc" in dv else ""
                p(f"   {r['name'][:20]:<20} {r['calls']:>6}  {human_time(r['time_s']['avg']):>10}  {gf}  {ipc}")
            p("   (time/rank is summed over a rank's threads for regions entered on several threads)")

    # OpenMP
    omp = (doc.get("metrics") or {}).get("openmp")
    if omp:
        p("")
        if not omp.get("available"):
            p(f" OpenMP                 not measured: {omp.get('reason')}")
        else:
            tt = omp["thread_time_s"]
            nr = run.get("nranks") or 1
            p(f" OpenMP                 {omp['runtime']}, {omp['threads'] / nr:g} threads/rank"
              + (f", {omp['ranks_without_ompt']} rank(s) without OMPT" if omp.get("ranks_without_ompt") else ""))
            p(f"   Serial time         {omp['serial_pct_of_elapsed']:5.1f}%   "
              f"(outside parallel regions, avg {human_time(omp['serial_s']['avg'])}/rank)")
            p(f"   Barrier imbalance   {omp['imbalance_pct']:5.1f}%   of thread time in parallel "
              f"regions ({human_time(tt['barrier_wait'] / nr)}/rank)")
            if tt["lock_wait"] > 0:
                p(f"   Lock wait           {omp['lock_wait_pct']:5.1f}%   (critical/locks, "
                  f"{human_time(tt['lock_wait'] / nr)}/rank)")
            if tt["task_wait"] > 0:
                p(f"   Taskwait            {human_time(tt['task_wait'] / nr)}/rank")
            regs = omp.get("regions") or []
            if regs:
                p("")
                p(" Parallel regions       calls/rank  wall/rank   team   imb%   location")
                for r in sorted(regs, key=lambda r: -r["wall_s_per_rank"])[:10]:
                    cpr = r["calls"] / r["ranks"] if r["ranks"] else 0
                    p(f"   {r['name'][:20]:<20} {cpr:>9g}  {human_time(r['wall_s_per_rank']):>10}"
                      f"  {r['avg_team']:5.1f}  {r['imbalance_pct']:5.1f}   {r.get('location', '')}")

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
                   "ranks may be bound to more cores than they use, or be idle/blocked.")
    if omp and omp.get("available"):
        if omp["serial_pct_of_elapsed"] > args.serial_pct:
            obs.append(f"{omp['serial_pct_of_elapsed']:.1f}% of the run is outside OpenMP parallel "
                       f"regions (> {args.serial_pct:g}%): only one thread per rank works there.")
        if omp["imbalance_pct"] > args.omp_imbalance_pct:
            worst = max(omp.get("regions") or [{}], key=lambda r: r.get("barrier_wait_s", 0))
            obs.append(f"OpenMP threads spend {omp['imbalance_pct']:.1f}% of their time waiting at "
                       f"barriers (> {args.omp_imbalance_pct:g}%)"
                       + (f"; most in {worst['name']}" if worst.get("name") else "")
                       + ": consider dynamic/guided scheduling or a finer work split.")
        if omp["lock_wait_pct"] > args.omp_imbalance_pct:
            obs.append(f"OpenMP lock/critical contention: {omp['lock_wait_pct']:.1f}% of thread time.")
    rem = cd.get("remote_dram_fill_pct")
    if rem is not None and rem > args.remote_pct:
        obs.append(f"{rem:.1f}% of DRAM fills come from a remote NUMA node (> {args.remote_pct:g}%): "
                   "check process/thread binding and first-touch initialisation.")
    if "packed_fp_ops_pct" in cd and cd["packed_fp_ops_pct"] < args.packed_pct:
        obs.append(f"Only {cd['packed_fp_ops_pct']:.1f}% of FP-unit ops are packed (< {args.packed_pct:g}%): "
                   "little use of vector instructions (includes FP data movement).")
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
    ap.add_argument("--remote-pct", type=float, default=20.0)
    ap.add_argument("--packed-pct", type=float, default=50.0)
    ap.add_argument("--serial-pct", type=float, default=20.0)
    ap.add_argument("--omp-imbalance-pct", type=float, default=10.0)
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
