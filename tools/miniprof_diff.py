#!/usr/bin/env python3
"""
miniprof_diff.py — explain the difference between two miniprof snapshots.

Usage:
    miniprof_diff.py A.json B.json [--label-a NAME] [--label-b NAME]
                     [--html OUT.html] [-o OUT.txt]

Typical uses: the same benchmark on two CPU generations, two compilers,
two rank/thread layouts, or before and after a change. B is compared
with A (ratios are B/A; "faster" means B took less time).

What it reports:
  - elapsed time and where it moved: compute / MPI / I/O per average rank,
    and each component's share of the change
  - compute time explained (needs the basic counter group in both): per
    average rank, T = instructions / (IPC x clock x busy cores), so
        T_B / T_A = instr ratio / (IPC ratio x clock ratio x busy-core ratio)
    exactly; each factor's share of the log ratio says which one drove the
    change (more or less work, faster cycles, higher clock, threading)
  - top-down level 1/2 of the compute phase, A vs B
  - MPI functions and user regions with the largest changes
  - node memory bandwidth, package power and energy to solution
  - roofline position of each run against its own ceilings
  - OpenMP serial and barrier-wait shares
Python 3 standard library only.
"""

import argparse
import json
import math
import os
import sys


def load(p):
    with open(p) as f:
        return json.load(f)


def g(d, *path, default=None):
    for k in path:
        if not isinstance(d, dict) or k not in d or d[k] is None:
            return default
        d = d[k]
    return d


def ratio(b, a):
    return b / a if a and b is not None and a > 0 else None


def pct_change(b, a):
    r = ratio(b, a)
    return None if r is None else 100.0 * (r - 1)


def fmt(x, f="{:.2f}", none="–"):
    return none if x is None else f.format(x)


def energy(j):
    """J below 10 kJ, kJ above; j in joules."""
    if j is None:
        return "–"
    return f"{j:.0f} J" if j < 1e4 else f"{j / 1e3:.1f} kJ"


def label_of(doc, path):
    n = (doc.get("nodes") or [{}])[0]
    cpu = g(n, "cpu", "uarch_name") or g(n, "cpu", "uarch") or "?"
    return f"{n.get('hostname', '?').split('.')[0]} ({cpu}), {g(doc, 'run', 'nranks')} ranks"


# ----------------------------------------------------------------- model

class Run:
    """The numbers the diff compares, pulled out of one snapshot."""

    def __init__(self, doc, label):
        self.doc, self.label = doc, label
        run = doc.get("run") or {}
        m = doc.get("metrics") or {}
        self.m = m
        self.nranks = run.get("nranks") or 1
        self.nnodes = run.get("nnodes") or 1
        self.elapsed = g(run, "elapsed_s", "max")
        par = m.get("parallel") or {}
        self.par = par
        ranks = doc.get("ranks") or []
        avg = lambda k: sum(r.get(k) or 0 for r in ranks) / len(ranks) if ranks else None
        self.comp, self.mpi, self.io = avg("compute_s"), avg("mpi_s"), avg("io_s")
        self.funcs = {f["name"]: f for f in g(par, "mpi", "by_function", default=[])}
        cnt = m.get("counters") or {}
        self.cnt = cnt if cnt.get("available") else {}
        self.cd = self.cnt.get("derived") or {}
        self.regions = {}
        for r in self.cnt.get("regions") or []:
            self.regions.setdefault(r["name"], {})["cnt"] = r
        td = m.get("topdown") or {}
        self.td = td if td.get("available") else {}
        for r in self.td.get("regions") or []:
            self.regions.setdefault(r["name"], {})["td"] = r
        roof = m.get("roofline") or {}
        self.roof = roof if roof.get("available") else {}
        for r in self.roof.get("regions") or []:
            self.regions.setdefault(r["name"], {})["roof"] = r
        mem = m.get("memory") or {}
        self.mem = mem if mem.get("available") else {}
        omp = m.get("openmp") or {}
        self.omp = omp if omp.get("available") else {}

    def region_time(self, name):
        r = self.regions.get(name) or {}
        if "cnt" in r:
            return g(r["cnt"], "time_s", "avg")
        if "td" in r:
            return r["td"].get("time_s_per_rank")
        if "roof" in r:
            return r["roof"].get("time_s_per_rank")
        return None

    def compute_factors(self):
        """Per average rank: instructions, IPC, clock (GHz), busy cores.
        T_compute = I / (IPC * f * P) holds by construction."""
        c = g(self.cnt, "compute") or {}
        cyc, ins = c.get("cycles"), c.get("instructions")
        f = self.cd.get("ghz_effective")
        T = self.comp
        if not (cyc and ins and f and T):
            return None
        I = ins / self.nranks
        ipc = ins / cyc
        P = cyc / self.nranks / (f * 1e9 * T)
        return {"instructions_per_rank": I, "ipc": ipc, "ghz": f, "busy_cores_per_rank": P}


# ----------------------------------------------------------------- analysis

def dram_gb(R):
    """Measured DRAM bytes moved in the whole run, all measured nodes (GB)."""
    tot = 0.0
    for nd in (R.mem.get("nodes") or []):
        tot += ((nd.get("read_gb_s") or 0) + (nd.get("write_gb_s") or 0)) * (nd.get("elapsed_s") or 0)
    return tot or None


def analyse(A: Run, B: Run) -> dict:
    out = {"A": A.label, "B": B.label}
    out["elapsed"] = (A.elapsed, B.elapsed, ratio(B.elapsed, A.elapsed))
    comps = []
    for k, a, b in (("compute", A.comp, B.comp), ("MPI", A.mpi, B.mpi), ("I/O", A.io, B.io)):
        comps.append((k, a, b, None if a is None or b is None else b - a))
    dsum = sum(d for _, _, _, d in comps if d is not None)
    out["components"] = [(k, a, b, d, (0.0 if d is not None and abs(d) < 1e-9 else
                                       100 * d / dsum if dsum and d is not None else None))
                         for k, a, b, d in comps]
    fa, fb = A.compute_factors(), B.compute_factors()
    if fa and fb:
        rows = []
        lt = math.log(B.comp / A.comp) if A.comp and B.comp else 0.0
        for key, name, sign in (("instructions_per_rank", "instructions per rank", +1),
                                ("ipc", "IPC (instructions per cycle)", -1),
                                ("ghz", "clock (effective GHz)", -1),
                                ("busy_cores_per_rank", "busy cores per rank", -1)):
            r = fb[key] / fa[key]
            contrib = sign * math.log(r)          # contribution to ln(T_B/T_A)
            # shares of a near-zero log ratio are meaningless (11000%): give
            # them only when compute changed by more than 2%
            sh = 100 * contrib / lt if abs(lt) > math.log(1.02) else None
            rows.append((name, fa[key], fb[key], r, contrib, 0.0 if sh is not None and abs(sh) < 0.5 else sh))
        out["factors"] = {"rows": rows, "time_ratio": B.comp / A.comp, "unchanged": abs(lt) <= math.log(1.02),
                          "check": math.exp(sum(r[4] for r in rows))}
    # top-down
    if A.td and B.td:
        la = dict(g(A.td, "compute", "level1", default={})); la.update(g(A.td, "compute", "level2", default={}))
        lb = dict(g(B.td, "compute", "level1", default={})); lb.update(g(B.td, "compute", "level2", default={}))
        keys = ["frontend_bound", "bad_speculation", "backend_bound", "retiring",
                "frontend_latency", "frontend_bandwidth", "backend_memory", "backend_cpu"]
        out["topdown"] = [(k, la.get(k), lb.get(k)) for k in keys if la.get(k) is not None or lb.get(k) is not None]
    # MPI functions by change in time per rank
    names = set(A.funcs) | set(B.funcs)
    fr = []
    for n in names:
        a = g(A.funcs.get(n), "time_s", "avg")
        b = g(B.funcs.get(n), "time_s", "avg")
        fr.append((n, a, b, (b or 0) - (a or 0),
                   g(A.funcs.get(n), "calls"), g(B.funcs.get(n), "calls")))
    out["mpi_functions"] = sorted(fr, key=lambda x: -abs(x[3]))
    # regions by change in time per rank
    rr = []
    for n in set(A.regions) | set(B.regions):
        a, b = A.region_time(n), B.region_time(n)
        gfa = g(A.regions.get(n), "cnt", "derived", "gflops_per_s_per_rank")
        gfb = g(B.regions.get(n), "cnt", "derived", "gflops_per_s_per_rank")
        ipa = g(A.regions.get(n), "cnt", "derived", "ipc")
        ipb = g(B.regions.get(n), "cnt", "derived", "ipc")
        rr.append((n, a, b, (b or 0) - (a or 0), ratio(b, a), gfa, gfb, ipa, ipb))
    out["regions"] = sorted(rr, key=lambda x: -abs(x[3]))
    # memory and energy
    if A.mem or B.mem:
        def memrow(R):
            pn = R.mem.get("per_node") or {}
            return {"read": g(pn, "read_gb_s", "avg"), "write": g(pn, "write_gb_s", "avg"),
                    "peak": pn.get("peak_gb_s_max"), "power": pn.get("power_w_avg"),
                    "energy_kj": (R.mem.get("energy_j_all_nodes") or 0) / 1e3 if R.mem else None,
                    "energy_j": R.mem.get("energy_j_all_nodes"),
                    "dram_gb": dram_gb(R),
                    "remote": R.mem.get("remote_read_share_pct"), "coverage": R.mem.get("coverage")}
        out["memory"] = (memrow(A) if A.mem else None, memrow(B) if B.mem else None)
    # roofline
    def roofrow(R):
        if not R.roof or not R.roof.get("nodes"):
            return None
        n = R.roof["nodes"][0]
        c = R.roof["ceilings"]
        return {"gflops": n["gflops_per_s"], "dram": n["dram_gb_s"], "ai": n["arithmetic_intensity"],
                "bound": n["bound"], "att": n["pct_of_attainable"], "peak": c["peak_gflops"],
                "peak_key": c["peak_key"], "roof_bw": n["dram_ceiling_gb_s"], "estimate": R.roof.get("estimate")}
    out["roofline"] = (roofrow(A), roofrow(B))
    if A.omp or B.omp:
        out["openmp"] = [(k, A.omp.get(k), B.omp.get(k)) for k in
                         ("serial_pct_of_elapsed", "imbalance_pct", "lock_wait_pct")]
    out["counters"] = [(k, A.cd.get(k), B.cd.get(k)) for k in
                       ("ipc", "ipc_compute", "ghz_effective", "gflops_per_s", "flops_per_cycle",
                        "dram_read_bytes", "dram_read_gb_per_s", "dram_fill_bytes", "dram_fill_gb_per_s",
                        "l2_misses_per_kilo_instr", "omp_idle_pct_of_cycles", "omp_idle_core_s_per_rank")
                       if A.cd.get(k) is not None or B.cd.get(k) is not None]
    out["verdict"] = verdict(A, B, out)
    return out


def verdict(A, B, d) -> list:
    v = []
    ea, eb, r = d["elapsed"]
    if r:
        if r < 0.98:
            v.append(f"B is {1 / r:.2f}x faster than A ({ea:.2f} s -> {eb:.2f} s, slowest rank).")
        elif r > 1.02:
            v.append(f"B is {r:.2f}x slower than A ({ea:.2f} s -> {eb:.2f} s, slowest rank).")
        else:
            v.append(f"A and B take the same time within 2% ({ea:.2f} s vs {eb:.2f} s).")
    comps = [c for c in d["components"] if c[3] is not None]
    if comps:
        k, a, b, dd, share = max(comps, key=lambda c: abs(c[3]))
        tot = sum(c[3] for c in comps)
        if abs(tot) > 1e-6:
            v.append(f"Per average rank the run {'lost' if tot > 0 else 'saved'} {abs(tot):.2f} s; "
                     f"{k} accounts for {abs(dd):.2f} s of it ({share:.0f}% of the change).")
    f = d.get("factors")
    if f and f["rows"] and f.get("unchanged"):
        moved = [r for r in f["rows"] if abs(r[4]) > math.log(1.05)]
        v.append(f"Compute time unchanged (x{f['time_ratio']:.2f})"
                 + (": " + ", ".join(f"{r[0]} x{r[3]:.2f}" for r in moved) + " moved but cancel out."
                    if moved else "."))
    elif f and f["rows"]:
        lead = max(f["rows"], key=lambda r: abs(r[4]))
        name, fa, fb, rr, contrib, share = lead
        direction = "shortened" if contrib < 0 else "lengthened"
        v.append(f"Compute time ratio {f['time_ratio']:.2f}: driven mostly by {name} "
                 f"({fa:.3g} -> {fb:.3g}, x{rr:.2f}), which {direction} it"
                 + (f" ({abs(share):.0f}% of the log change)." if share is not None else "."))
        if lead[0].startswith("IPC"):
            mem = d.get("memory") or (None, None)
            pairs = []
            if mem[0] and mem[1] and mem[0].get("dram_gb") and mem[1].get("dram_gb") \
                    and not (mem[0].get("coverage") or mem[1].get("coverage")):
                pairs.append(("DRAM traffic (measured, reads + writes)", mem[0]["dram_gb"] * 1e9, mem[1]["dram_gb"] * 1e9))
            for key, what in (("dram_read_bytes", "DRAM reads (core counters)"),
                              ("dram_fill_bytes", "L1D fills from DRAM")):
                if A.cd.get(key) and B.cd.get(key):
                    pairs.append((what, A.cd[key], B.cd[key]))
            if pairs and abs(pairs[0][2] / pairs[0][1] - 1) > 0.10:
                what, ba, bb = pairs[0]
                v.append(f"Same work, {what}: {ba / 1e9:.1f} GB in A vs {bb / 1e9:.1f} GB in B (x{bb / ba:.2f}). "
                         "Fewer bytes from DRAM for the same instructions means more of the working set "
                         "stayed in cache, which raises IPC.")
    regs = [x for x in d["regions"] if x[1] is not None and x[2] is not None]
    if regs:
        n, a, b, dd, rr, *_ = regs[0]
        v.append(f"Largest region change: {n}, {a:.2f} s -> {b:.2f} s per rank ({dd:+.2f} s, x{rr:.2f}).")
    fn = [x for x in d["mpi_functions"] if abs(x[3]) > 0.01]
    if fn:
        n, a, b, dd, *_ = fn[0]
        v.append(f"Largest MPI change: {n}, {fmt(a)} s -> {fmt(b)} s per rank ({dd:+.2f} s).")
    td = d.get("topdown")
    if td:
        l1 = [x for x in td if x[0] in ("frontend_bound", "bad_speculation", "backend_bound", "retiring")
              and x[1] is not None and x[2] is not None]
        if l1:
            k, a, b = max(l1, key=lambda x: abs(x[2] - x[1]))
            bm = next((x for x in td if x[0] == "backend_memory"), None)
            extra = (f"; backend-memory {bm[1]:.0f}% -> {bm[2]:.0f}%" if bm and bm[1] is not None and bm[2] is not None
                     and k == "backend_bound" else "")
            v.append(f"Top-down (compute): largest shift is {k.replace('_', ' ')}, {a:.0f}% -> {b:.0f}% of slots{extra}.")
    for tag, x in zip("AB", d["roofline"]):
        if x and x["bound"] == "memory" and not x["estimate"] and x["att"] < 50:
            v.append(f"{tag} is below both roofs: memory side of the ridge but at {x['att']:.0f}% of its DRAM "
                     "roof over the whole run, so bandwidth is not the limit (latency or the core itself, "
                     "or much of the run spent outside compute).")
    mem = d.get("memory")
    if mem and mem[0] and mem[1] and mem[0].get("energy_j") and mem[1].get("energy_j"):
        er = mem[1]["energy_j"] / mem[0]["energy_j"]
        v.append(f"Energy to solution (package, all nodes): {energy(mem[0]['energy_j'])} -> "
                 f"{energy(mem[1]['energy_j'])} (x{er:.2f}) at {mem[0]['power'] or 0:.0f} W -> {mem[1]['power'] or 0:.0f} W.")
    if mem and ((mem[0] or {}).get("coverage") or (mem[1] or {}).get("coverage")):
        v.append("Memory figures for a run with partial coverage count part of the node only (see memory).")
    return v


# ----------------------------------------------------------------- text

def text(d, A, B) -> str:
    L = []
    p = L.append
    W = 78
    p("=" * W)
    p(" miniprof diff  (B compared with A; ratios are B/A)")
    for tag, R in (("A", A), ("B", B)):
        run = R.doc.get("run") or {}
        p(f"   {tag}  {R.label}")
        p(f"      {run.get('command', '')[:68]}")
        p(f"      {run.get('nranks')} ranks on {run.get('nnodes')} node(s), level {run.get('level')}, "
          f"{run.get('start_utc', '')}")
    p("=" * W)
    p("")
    for s in d["verdict"]:
        p(f" * {s}")
    p("")
    p(" Where the time went (average rank)      A            B          change   share")
    for k, a, b, dd, share in d["components"]:
        p(f"   {k:<14} {fmt(a, '{:10.2f} s'):>22} {fmt(b, '{:10.2f} s'):>12} "
          f"{fmt(dd, '{:+9.2f} s'):>11} {fmt(share, '{:6.0f}%'):>7}")
    ea, eb, r = d["elapsed"]
    p(f"   {'elapsed (max)':<14} {fmt(ea, '{:10.2f} s'):>22} {fmt(eb, '{:10.2f} s'):>12}   x{fmt(r)}")
    f = d.get("factors")
    if f:
        p("")
        p(" Compute time explained: T = instructions / (IPC x clock x busy cores), per rank")
        p("                                   A              B         B/A   share of change")
        for name, fa, fb, rr, contrib, share in f["rows"]:
            p(f"   {name:<28} {fa:12.4g} {fb:14.4g} {rr:9.3f} {fmt(share, '{:12.0f}%'):>13}")
        p(f"   {'compute time ratio':<28} {'':12} {'':14} {f['time_ratio']:9.3f}   "
          f"(product of the factors: {f['check']:.3f})")
        p("   shares are of ln(T_B/T_A): negative = that factor worked against the change"
          if not f.get("unchanged") else "   compute time within 2%: no shares (they would divide by ~0)")
    if d.get("counters"):
        p("")
        p(" Counters (whole run)                    A            B         B/A")
        for k, a, b in d["counters"]:
            p(f"   {k:<30} {fmt(a, '{:10.3g}'):>11} {fmt(b, '{:12.3g}'):>12} {fmt(ratio(b, a), '{:10.3f}'):>11}")
    if d.get("topdown"):
        p("")
        p(" Top-down, compute (% of slots)          A            B      change")
        for k, a, b in d["topdown"]:
            p(f"   {k:<30} {fmt(a, '{:10.1f}'):>11} {fmt(b, '{:12.1f}'):>12} "
              f"{fmt(None if a is None or b is None else b - a, '{:+10.1f}'):>11}")
    regs = [x for x in d["regions"]][:12]
    if regs:
        p("")
        p(" Regions (time per rank)          A          B     change     B/A   GFLOP/s/rank A -> B")
        for n, a, b, dd, rr, gfa, gfb, ipa, ipb in regs:
            p(f"   {n[:22]:<22} {fmt(a, '{:9.3f}'):>10} {fmt(b, '{:10.3f}'):>10} {dd:+10.3f} "
              f"{fmt(rr, '{:7.2f}'):>7}   {fmt(gfa, '{:.2f}')} -> {fmt(gfb, '{:.2f}')}")
    fn = d["mpi_functions"][:10]
    if fn:
        p("")
        p(" MPI functions (time per rank)    A          B     change    calls A -> B")
        for n, a, b, dd, ca, cb in fn:
            p(f"   {n[:22]:<22} {fmt(a, '{:9.3f}'):>10} {fmt(b, '{:10.3f}'):>10} {dd:+10.3f}    "
              f"{fmt(ca, '{:,}')} -> {fmt(cb, '{:,}')}")
    mem = d.get("memory")
    if mem:
        p("")
        p(" Memory (per node, average)              A            B         B/A")
        ma, mb = mem[0] or {}, mem[1] or {}
        for k, lab in (("read", "DRAM read GB/s"), ("write", "DRAM write GB/s"), ("peak", "peak GB/s"),
                       ("dram_gb", "DRAM moved, whole run GB"), ("power", "package power W"),
                       ("remote", "remote reads %")):
            a, b = ma.get(k), mb.get(k)
            p(f"   {lab:<30} {fmt(a, '{:10.1f}'):>11} {fmt(b, '{:12.1f}'):>12} {fmt(ratio(b, a), '{:10.3f}'):>11}")
        p(f"   {'energy, all nodes':<30} {energy(ma.get('energy_j')):>11} {energy(mb.get('energy_j')):>12} "
          f"{fmt(ratio(mb.get('energy_j'), ma.get('energy_j')), '{:10.3f}'):>11}")
        for tag, mm in (("A", ma), ("B", mb)):
            if mm.get("coverage"):
                p(f"   ! {tag}: partial coverage: {mm['coverage']}")
    ra, rb = d["roofline"]
    if ra or rb:
        p("")
        p(" Roofline (node 0, each against its own ceilings)")
        for tag, x in (("A", ra), ("B", rb)):
            if not x:
                p(f"   {tag}  not available")
                continue
            regime = "below both roofs" if x["att"] < 50 else f"{x['bound']}-bound"
            p(f"   {tag}  {x['gflops']:.1f} GFLOP/s at {x['ai']:.3g} FLOP/B, {regime} ({x['bound']} side), "
              f"{x['att']:.0f}% of attainable (peak {x['peak']:.0f} {x['peak_key']}, DRAM {x['roof_bw']:.0f} GB/s)"
              + ("  [estimate]" if x["estimate"] else ""))
    if d.get("openmp"):
        p("")
        p(" OpenMP                                  A            B")
        for k, a, b in d["openmp"]:
            p(f"   {k:<30} {fmt(a, '{:9.1f}%'):>11} {fmt(b, '{:11.1f}%'):>12}")
    p("=" * W)
    return "\n".join(L) + "\n"


# ----------------------------------------------------------------- html

def html(d, A, B) -> str:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import miniprof_html as H
    esc = H.esc
    S = []
    nav = []

    def section(key, title, body):
        nav.append((key, title))
        S.append(f'<section id="{key}"><h2>{esc(title)}</h2>{body}</section>')

    ea, eb, r = d["elapsed"]
    tiles = [H.tile("A elapsed", H.ht(ea), A.label), H.tile("B elapsed", H.ht(eb), B.label)]
    if r:
        tiles.append(H.tile("B vs A", f"{1 / r:.2f}x faster" if r < 1 else f"{r:.2f}x slower", "slowest rank"))
    mem = d.get("memory")
    if mem and mem[0] and mem[1] and mem[0].get("energy_j") and mem[1].get("energy_j"):
        tiles.append(H.tile("Energy B/A", f'x{mem[1]["energy_j"] / mem[0]["energy_j"]:.2f}',
                            f'{energy(mem[0]["energy_j"])} → {energy(mem[1]["energy_j"])}'))
    f = d.get("factors")
    if f:
        tiles.append(H.tile("Compute time B/A", f"x{f['time_ratio']:.2f}", "average rank"))

    items = "".join(f'<li><span class="ic" aria-hidden="true">→</span><span>{esc(s)}</span></li>' for s in d["verdict"])
    S.append(f'<section id="verdict"><h2>Summary</h2><ul class="obs vd">{items}</ul></section>')

    names = ("A", "B")
    body = H.legend([(1, f"A: {A.label}"), (2, f"B: {B.label}")])
    body += H.paired_bars([(k, a, b) for k, a, b, _, _ in d["components"]] + [("elapsed (max)", ea, eb)], names)
    body += H.table(["", "A", "B", "change", "share of change"],
                    [(k, H.ht(a), H.ht(b), H.num(dd, "{:+.2f} s"), H.num(sh, "{:.0f}%"))
                     for k, a, b, dd, sh in d["components"]])
    section("time", "Where the time went (average rank)", body)

    if f:
        body = '<p class="note">Per average rank, compute time T = instructions / (IPC × clock × busy cores) ' \
               'exactly, so T<sub>B</sub>/T<sub>A</sub> is the product of the factor ratios below (an ' \
               'IPC, clock or busy-core ratio above 1 shortens T). The share is each factor\'s part of ' \
               'ln(T<sub>B</sub>/T<sub>A</sub>); a negative share worked against the change.</p>'
        body += H.table(["factor", "A", "B", "B/A", "share of change"],
                        [(n, f"{fa:.4g}", f"{fb:.4g}", f"{rr:.3f}", H.num(sh, "{:.0f}%"))
                         for n, fa, fb, rr, c, sh in f["rows"]]
                        + [("compute time ratio", "", "", f"{f['time_ratio']:.3f}", "")])
        section("factors", "Compute time explained", body)

    if d.get("counters"):
        section("counters", "Counters (whole run)", H.table(
            ["metric", "A", "B", "B/A"],
            [(k, H.num(a, "{:.3g}"), H.num(b, "{:.3g}"), H.num(ratio(b, a), "{:.3f}")) for k, a, b in d["counters"]]))

    if d.get("topdown"):
        keys = [(1, "frontend_bound", "Frontend bound"), (2, "bad_speculation", "Bad speculation"),
                (3, "backend_bound", "Backend bound"), (4, "retiring", "Retiring")]
        la = {k: a for k, a, b in d["topdown"]}
        lb = {k: b for k, a, b in d["topdown"]}
        body = H.legend([(s, n) for s, _, n in keys])
        body += H.hstack([("A", la, None), ("B", lb, None)], keys, vmax=100, label_w=60)
        body += H.table(["% of slots", "A", "B", "change"],
                        [(k, H.num(a), H.num(b), H.num(None if a is None or b is None else b - a, "{:+.1f}"))
                         for k, a, b in d["topdown"]])
        section("topdown", "Top-down, compute phase", body)

    regs = [x for x in d["regions"] if x[1] is not None or x[2] is not None][:12]
    if regs:
        body = H.legend([(1, "A"), (2, "B")])
        body += H.paired_bars([(n, a, b) for n, a, b, *_ in regs], names)
        body += H.table(["region", "A", "B", "change", "B/A", "GFLOP/s/rank A", "B", "IPC A", "B"],
                        [(n, H.ht(a), H.ht(b), f"{dd:+.3f} s", H.num(rr, "{:.2f}"), H.num(gfa, "{:.2f}"),
                          H.num(gfb, "{:.2f}"), H.num(ipa, "{:.2f}"), H.num(ipb, "{:.2f}"))
                         for n, a, b, dd, rr, gfa, gfb, ipa, ipb in regs])
        section("regions", "Regions (time per rank)", body)

    fn = d["mpi_functions"][:12]
    if fn:
        body = H.legend([(1, "A"), (2, "B")])
        body += H.paired_bars([(n, a, b) for n, a, b, *_ in fn], names, fmt="{:.3f}")
        body += H.table(["function", "A", "B", "change", "calls A", "calls B"],
                        [(n, H.ht(a), H.ht(b), f"{dd:+.3f} s", H.num(ca, "{:,}"), H.num(cb, "{:,}"))
                         for n, a, b, dd, ca, cb in fn])
        section("mpi", "MPI functions (time per rank)", body)

    if mem:
        ma, mb = mem[0] or {}, mem[1] or {}
        rows = []
        for k, lab, u in (("read", "DRAM read", " GB/s"), ("write", "DRAM write", " GB/s"), ("peak", "peak", " GB/s"),
                          ("dram_gb", "DRAM moved, whole run", " GB"), ("power", "package power", " W"),
                          ("remote", "remote reads", "%")):
            rows.append((lab, H.num(ma.get(k), "{:.1f}" + u), H.num(mb.get(k), "{:.1f}" + u),
                         H.num(ratio(mb.get(k), ma.get(k)), "{:.3f}")))
        rows.append(("energy, all nodes", energy(ma.get("energy_j")), energy(mb.get("energy_j")),
                     H.num(ratio(mb.get("energy_j"), ma.get("energy_j")), "{:.3f}")))
        body = H.table(["per node, average", "A", "B", "B/A"], rows)
        for tag, mm in (("A", ma), ("B", mb)):
            if mm.get("coverage"):
                body += f'<p class="note"><b>{tag}: partial coverage.</b> {esc(mm["coverage"])}.</p>'
        section("memory", "Memory and energy", body)

    ra, rb = d["roofline"]
    if ra and rb:
        pts = [("A", ra["ai"], ra["gflops"], 1), ("B", rb["ai"], rb["gflops"], 2)]
        same = abs(ra["peak"] - rb["peak"]) / ra["peak"] < 0.02 and abs(ra["roof_bw"] - rb["roof_bw"]) / ra["roof_bw"] < 0.02
        body = H.legend([(1, f"A: {A.label}"), (2, f"B: {B.label}")])
        body += H.roofline_chart({"peak": ra["peak"], "peak_key": ra["peak_key"], "all_gflops": None,
                                  "dram": ra["roof_bw"]}, pts,
                                 ceil2=None if same else {"peak": rb["peak"], "dram": rb["roof_bw"], "label": "B"})
        if not same:
            body += '<p class="note">Solid roof: A\'s ceilings. Dashed: B\'s.</p>'
        body += H.table(["", "GFLOP/s", "DRAM GB/s", "FLOP/byte", "bound", "of attainable", "estimate"],
                        [(t, f'{x["gflops"]:.1f}', f'{x["dram"]:.1f}', f'{x["ai"]:.3g}', x["bound"],
                          f'{x["att"]:.0f}%', "yes" if x["estimate"] else "") for t, x in (("A", ra), ("B", rb))],
                        text=(4, 6))
        section("roofline", "Roofline (node 0)", body)

    if d.get("openmp"):
        section("openmp", "OpenMP", H.table(["", "A", "B"],
                                            [(k, H.num(a, "{:.1f}%"), H.num(b, "{:.1f}%")) for k, a, b in d["openmp"]]))

    runs = []
    for tag, R in (("A", A), ("B", B)):
        run = R.doc.get("run") or {}
        runs.append((tag, R.label, run.get("command"), f'{run.get("nranks")} on {run.get("nnodes")}',
                     run.get("level"), run.get("start_utc")))
    section("runs", "Runs", H.table(["", "label", "command", "ranks on nodes", "level", "started (UTC)"], runs,
                                    text=(1, 2, 4, 5)))

    navh = "".join(f'<a href="#{k}">{esc(t)}</a>' for k, t in nav)
    title = f"miniprof diff — {A.label} vs {B.label}"
    css = H.CSS + ".vd .ic{background:var(--s1);color:#fff}"
    return ("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            f"<title>{esc(title)}</title><style>{css}</style></head><body><div class=\"mp\"><div class=\"wrap\">"
            f"<header><h1>miniprof diff</h1><div class=\"meta\">A: {esc(A.label)} · B: {esc(B.label)} · "
            f"ratios are B/A</div><nav>{navh}</nav></header>"
            f'<div class="tiles">{"".join(tiles)}</div>' + "".join(S) +
            '<footer>generated by miniprof_diff.py</footer>'
            f'</div><div id="tip" role="tooltip"></div></div><script>{H.JS}</script></body></html>\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--label-a")
    ap.add_argument("--label-b")
    ap.add_argument("--html", metavar="OUT.html")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    da, db = load(a.a), load(a.b)
    la = a.label_a or label_of(da, a.a)
    lb = a.label_b or label_of(db, a.b)
    if la == lb:
        la, lb = f"{la} [{os.path.basename(a.a)}]", f"{lb} [{os.path.basename(a.b)}]"
    A, B = Run(da, la), Run(db, lb)
    d = analyse(A, B)
    t = text(d, A, B)
    if a.html:
        with open(a.html, "w") as f:
            f.write(html(d, A, B))
    if a.output:
        with open(a.output, "w") as f:
            f.write(t)
    else:
        sys.stdout.write(t)
    return 0


if __name__ == "__main__":
    sys.exit(main())
