"""appcheck.py — shared checks for miniprof validation on real applications
(check_wrf.py, check_lammps.py).

A validation directory holds NN_<level>/ run directories, each with
wall_s.txt, out_md5.txt / out_size.txt (output files) and, for profiled
runs, snapshot.json plus report.txt/report.html/csv. The application
checker parses its own logs and calls these for everything else.
"""
import glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OVERHEAD = {"basic": 2.0, "topdown": 5.0, "roofline": 5.0, "full": 5.0}  # % of wall


class Report:
    def __init__(self): self.fails = []
    def ok(self, m):   print("PASS: " + m)
    def warn(self, m): print("WARN: " + m)
    def info(self, m): print("      " + m)
    def bad(self, m):  print("FAIL: " + m); self.fails.append(m)
    def check(self, cond, m): (self.ok if cond else self.bad)(m); return cond
    def section(self, m): print("\n== " + m)
    def finish(self, what):
        print()
        if self.fails:
            print(f"{len(self.fails)} CHECK(S) FAILED"); return 1
        print(f"ALL {what} CHECKS PASSED"); return 0


def read(path, default=None):
    try:
        with open(path) as f: return f.read()
    except OSError: return default


def files(path):
    """{name: value} from 'value name' lines (md5sum / stat output)."""
    out = {}
    for line in (read(path, "") or "").splitlines():
        p = line.split()
        if len(p) >= 2: out[os.path.basename(p[-1])] = p[0]
    return out


def g(d, *keys, default=None):
    for k in keys:
        if not isinstance(d, dict) or k not in d: return default
        d = d[k]
    return d


def env_omp(top):
    m = re.search(r"OMP_NUM_THREADS=(\d+)", read(os.path.join(top, "environment.txt"), "") or "")
    return int(m.group(1)) if m else None


def load_runs(top, rep, md5="out_md5.txt", size="out_size.txt"):
    runs = []
    for d in sorted(glob.glob(os.path.join(top, "[0-9][0-9]_*"))):
        name = os.path.basename(d).split("_", 1)[1]
        w = read(os.path.join(d, "wall_s.txt"))
        snap = None
        if os.path.exists(os.path.join(d, "snapshot.json")):
            try:
                with open(os.path.join(d, "snapshot.json")) as f: snap = json.load(f)
            except ValueError as e:
                rep.bad(f"{os.path.basename(d)}: snapshot.json unreadable ({e})")
        runs.append({"dir": d, "id": os.path.basename(d), "level": name,
                     "wall": float(w) if w else None,
                     "md5": files(os.path.join(d, md5)),
                     "size": {k: int(v) for k, v in files(os.path.join(d, size)).items()},
                     "snap": snap})
    return runs


def check_overhead(rep, base, prof, inner=None, inner_name="main loop", runs=None):
    """Wall-time overhead of each profiled run against the unprofiled runs
    next to it in the sequence (the validators interleave them, so a node
    that drifts over the session does not count as overhead). The limit grows
    by the spread between consecutive unprofiled runs. inner(r) is the
    application's own timer for its main loop, shown alongside."""
    rep.section("overhead (wall time, profiled vs neighbouring unprofiled runs)")
    if not base:
        rep.bad("no successful unprofiled ('off') run to compare against"); return
    order = runs or (base + prof)
    pos = {r["id"]: i for i, r in enumerate(order)}
    offs = sorted(base, key=lambda r: pos[r["id"]])
    noise = 0.0
    for a_, b_ in zip(offs, offs[1:]):
        noise = max(noise, 100.0 * abs(a_["wall"] - b_["wall"]) / min(a_["wall"], b_["wall"]))
    rep.info(f"unprofiled runs: {', '.join('%.1f' % r['wall'] for r in offs)} s; "
             f"largest step between neighbours {noise:.1f}%")
    for r in prof:
        if not r["wall"]: continue
        i = pos[r["id"]]
        near = [o for o in offs if pos[o["id"]] == max([pos[x["id"]] for x in offs if pos[x["id"]] < i], default=-1)]
        near += [o for o in offs if pos[o["id"]] == min([pos[x["id"]] for x in offs if pos[x["id"]] > i], default=-1)]
        near = near or offs
        ref = sum(o["wall"] for o in near) / len(near)
        ov = 100.0 * (r["wall"] - ref) / ref
        extra = ""
        if inner:
            ref_in = sum(inner(o) for o in near) / len(near)
            if ref_in:
                extra = f" ({inner_name} {100.0 * (inner(r) - ref_in) / ref_in:+.1f}%)"
        lim = OVERHEAD.get(r["level"], 5.0)
        rep.check(ov <= lim + noise,
                  f"{r['id']}: wall {ov:+.1f}% against {', '.join(o['id'] for o in near)}{extra}, "
                  f"limit {lim:.0f}% + noise {noise:.1f}%")


def check_identical(rep, base, prof, key, what):
    """key(r) -> {name: digest}; every profiled run must match the first
    unprofiled run, if the unprofiled runs agree with each other."""
    rep.section("results unchanged")
    if not base or not key(base[0]):
        rep.warn(f"no {what} from an unprofiled run"); return
    ref = key(base[0])
    if len(base) > 1 and key(base[1]) != ref:
        diff = sorted(k for k in ref if key(base[1]).get(k) != ref[k])
        rep.warn(f"the two unprofiled runs differ from each other in {', '.join(diff[:4])} "
                 "(non-reproducible build or threading): comparison skipped")
        return
    for r in prof:
        k = key(r)
        if not k: rep.bad(f"{r['id']}: no {what}"); continue
        diff = sorted(n for n in ref if k.get(n) != ref[n])
        rep.check(not diff and set(k) == set(ref),
                  f"{r['id']}: {len(ref)} {what} identical to the unprofiled run"
                  + (f" (differ: {', '.join(diff[:4])})" if diff else ""))


def check_shape(rep, s, omp, ranks=None, threads=None):
    """Ranks and threads in the snapshot against the application's own count."""
    M = s.get("metrics", {})
    nr = g(s, "run", "nranks")
    if ranks:
        rep.check(nr == ranks, f"ranks {nr} == the application's {ranks}")
    rep.info(f"{nr} ranks on {g(s, 'run', 'nnodes')} nodes, {g(s, 'run', 'ranks_per_node', 'max')} per node, "
             f"{g(s, 'run', 'cpus_per_rank', 'min')}-{g(s, 'run', 'cpus_per_rank', 'max')} CPUs per rank, "
             f"MPI thread level {g(s, 'run', 'mpi_thread_level')}, "
             f"{str(g(s, 'run', 'mpi_library')).split(',')[0]}")
    ot = g(s, "run", "omp_num_threads")
    try: ot = int(ot)
    except (TypeError, ValueError): ot = None
    if omp:
        rep.check(ot == omp, f"omp_num_threads {ot} == OMP_NUM_THREADS {omp}")
    if threads and ot:
        rep.check(threads == ot, f"OpenMP threads {ot} == the application's {threads}")
    cpr = g(s, "run", "cpus_per_rank", "min")
    if omp and cpr is not None:
        rep.check(cpr >= omp, f"each rank may run on >= {omp} CPUs (min {cpr}): threads not stacked on one core")
    om = M.get("openmp", {})
    if om.get("available"):
        rep.check(om.get("threads") == nr * (ot or 1),
                  f"OpenMP threads seen {om.get('threads')} == ranks x threads {nr * (ot or 1)}")
        rep.info(f"OpenMP parallel {om.get('parallel_pct_of_elapsed')}% of elapsed, thread imbalance "
                 f"{om.get('imbalance_pct')}%, runtime {om.get('runtime')}")
    else:
        rep.warn(f"OpenMP block unavailable: {om.get('reason') or 'no OMPT in the runtime'}")
    return nr, ot


def time_split(rep, s):
    split = g(s, "metrics", "parallel", "time_split_pct", default={}) or {}
    rep.info("time split: compute %.1f%%  MPI %.1f%%  I/O %.1f%%" % tuple(
        split.get(k) or 0 for k in ("compute", "mpi", "io")))
    rep.check(abs(sum(v or 0 for v in split.values()) - 100) < 1.5, "time split sums to 100%")
    return split


def mpi_summary(rep, s, steps=None):
    mpi = g(s, "metrics", "parallel", "mpi", default={}) or {}
    nr = g(s, "run", "nranks") or 1
    if nr == 1:
        rep.info("single rank: MPI checks skipped"); return mpi
    p2p = g(mpi, "by_class", "p2p", "calls", default=0) or 0
    rep.check(p2p > 0, f"halo exchange seen: {p2p} point-to-point calls")
    if steps:
        rep.info(f"{p2p / nr / steps:.0f} p2p calls per rank per step")
    rep.info("top MPI: " + ", ".join("%s %.1f%%" % (f["name"], f["pct_of_elapsed"] or 0)
                                     for f in mpi.get("by_function", [])[:5]))
    rep.check(0.5 <= (mpi.get("pct_of_elapsed") or 0) <= 60,
              "MPI %.1f%% of elapsed (plausible 0.5-60%%), imbalance %.1f%%"
              % (mpi.get("pct_of_elapsed") or 0, mpi.get("imbalance_pct") or 0))
    return mpi


def check_hardware(rep, r):
    """Counters, topdown, memory and roofline blocks, and the tool outputs."""
    s, lvl = r["snap"], r["level"]
    M = s.get("metrics", {})
    c = M.get("counters", {})
    if c.get("available"):
        rep.check(not c.get("ranks_without_counters"),
                  f"counters on every rank ({c.get('uarch')}, group {c.get('group')}, read {c.get('read_method')})")
        rep.check(not c.get("threads_dropped"),
                  f"threads counted {c.get('threads_counted')}, dropped {c.get('threads_dropped')}")
        if c.get("multiplexed_ranks"):
            rep.warn(f"{c['multiplexed_ranks']} ranks multiplexed (NMI watchdog or another perf user?)")
        d = c.get("derived", {})
        ipc = d.get("ipc_compute") or d.get("ipc")
        rep.check(bool(ipc) and 0.2 <= ipc <= 5,
                  f"IPC in compute {ipc}, GHz {d.get('ghz_effective')}, GFLOP/s {d.get('gflops_per_s')}")
        if lvl in ("basic", "roofline") and d.get("flops_in_mpi_pct") is not None:
            rep.check(d["flops_in_mpi_pct"] < 2, f"FLOPs inside MPI {d['flops_in_mpi_pct']}% (should be ~0)")
    else:
        rep.bad(f"counters unavailable: {c.get('reason')}")

    if lvl == "topdown":
        td = M.get("topdown", {})
        if td.get("available"):
            l1 = g(td, "compute", "level1", default={}) or {}
            tot = sum(v for v in l1.values() if isinstance(v, (int, float)))
            rep.check(97 <= tot <= 103, "topdown level 1 sums to %.1f%%: %s" % (
                tot, ", ".join(f"{k} {v}" for k, v in l1.items() if v is not None)))
        else:
            rep.bad(f"topdown unavailable: {td.get('reason')}")

    mem = M.get("memory", {})
    if mem.get("available"):
        rep.check(not mem.get("nodes_without"),
                  f"memory measured on {mem.get('nodes_measured')} nodes ({mem.get('nodes_without')} without)")
        rep.info(f"DRAM {mem.get('total_gb_s_all_nodes')} GB/s all nodes, energy "
                 f"{mem.get('energy_j_all_nodes')} J, remote reads {mem.get('remote_read_share_pct')}%")
    else:
        rep.warn(f"memory block unavailable: {mem.get('reason')} "
                 "(setcap on every node, or perf_event_paranoid <= 0)")

    if lvl == "roofline":
        rf = M.get("roofline", {})
        if rf.get("available"):
            n0 = (rf.get("nodes") or [{}])[0]
            rep.info(f"roofline node 0: {n0.get('gflops_per_s')} GFLOP/s, {n0.get('dram_gb_s')} GB/s, "
                     f"AI {n0.get('arithmetic_intensity')}, {n0.get('bound')} bound, "
                     f"{n0.get('pct_of_attainable')}% of attainable")
            rep.check(0 < (n0.get("pct_of_attainable") or 0) <= 110, "roofline point under its roof (<= 110%)")
            regs = rf.get("regions") or []
            rep.info(f"{len(regs)} roofline regions" if regs else f"no per-region roofline: {rf.get('regions_reason')}")
        elif rf.get("estimate"):
            rep.warn(f"roofline is an estimate only: {rf.get('reason')}")
        else:
            rep.bad(f"roofline unavailable: {rf.get('reason')}")

    for f in ("report.txt", "report.html", "csv"):
        rep.check(os.path.exists(os.path.join(r["dir"], f)), f"{f} written")


def check_diff(rep, top, good):
    rep.section("miniprof_diff at scale")
    if len(good) < 2:
        rep.warn("fewer than two snapshots: diff not exercised"); return
    a, b = good[0], good[-1]
    out = os.path.join(top, f"diff_{a['level']}_{b['level']}.html")
    p = subprocess.run([sys.executable, os.path.join(HERE, "miniprof_diff.py"),
                        os.path.join(a["dir"], "snapshot.json"), os.path.join(b["dir"], "snapshot.json"),
                        "--label-a", a["level"], "--label-b", b["level"], "--html", out],
                       capture_output=True, text=True)
    rep.check(p.returncode == 0 and os.path.exists(out), f"diff {a['level']} vs {b['level']} -> {out}")
    for line in p.stdout.splitlines()[:6]: rep.info(line)
    ea, eb = g(a["snap"], "run", "elapsed_s", "max"), g(b["snap"], "run", "elapsed_s", "max")
    if ea and eb:
        rep.info(f"same application at two levels: elapsed ratio {eb / ea:.3f} "
                 "(differences are profiling overhead plus run-to-run noise)")
