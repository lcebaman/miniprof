#!/usr/bin/env python3
"""check_wrf.py — check miniprof snapshots of a WRF run against WRF itself.

Reads the directory written by tools/validate_wrf.sh:
  environment.txt, input_size.txt, NN_<run>/{wall_s.txt, rsl.error.0000,
  wrfout_md5.txt, wrfout_size.txt, snapshot.json, report.*}
and checks, for every profiled run:
  overhead   wall time against the unprofiled runs (and their own spread)
  results    output files byte-identical to the unprofiled run
  shape      ranks and threads in the snapshot match WRF's decomposition
  time       elapsed covers WRF's own timed steps; I/O time against WRF's
             "Timing for Writing" and bytes against the wrfout sizes
  MPI        halo exchange (p2p + wait) present, shares plausible
  counters   no dropped threads, IPC and topdown sane, memory and roofline
  tools      report, HTML, CSV written; miniprof_diff runs on two levels
Exit 0 = all checks passed (WARN lines do not fail).
"""
import glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OVERHEAD = {"basic": 2.0, "topdown": 5.0, "roofline": 5.0, "full": 5.0}  # % of wall

fails = []
def ok(msg):   print("PASS: " + msg)
def warn(msg): print("WARN: " + msg)
def info(msg): print("      " + msg)
def bad(msg):  print("FAIL: " + msg); fails.append(msg)
def check(cond, msg): (ok if cond else bad)(msg)


def read(path, default=None):
    try:
        with open(path) as f: return f.read()
    except OSError: return default


def rsl(path):
    """WRF's own timings and decomposition from rsl.error.0000."""
    t = read(path, "")
    num = r"([0-9.]+) elapsed seconds"
    r = {
        "success": "SUCCESS COMPLETE WRF" in t,
        "main_s": sum(float(x) for x in re.findall(r"Timing for main:.*?" + num, t)),
        "main_steps": len(re.findall(r"Timing for main:", t)),
        "write_s": sum(float(x) for x in re.findall(r"Timing for Writing.*?" + num, t)),
        "writes": len(re.findall(r"Timing for Writing", t)),
        "input_s": sum(float(x) for x in re.findall(r"Timing for processing.*?" + num, t)),
    }
    m = re.search(r"Ntasks in X\s+(\d+)\s*,\s*ntasks in Y\s+(\d+)", t)
    r["ntasks"] = int(m.group(1)) * int(m.group(2)) if m else None
    m = re.search(r"NUMBER OF TILES\s*=\s*(\d+)", t)
    r["tiles"] = int(m.group(1)) if m else None
    return r


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


def main():
    if len(sys.argv) != 2:
        print(__doc__); return 2
    top = sys.argv[1]
    env = read(os.path.join(top, "environment.txt"), "")
    m = re.search(r"OMP_NUM_THREADS=(\d+)", env)
    omp = int(m.group(1)) if m else None
    inputs = {k: int(v) for k, v in files(os.path.join(top, "input_size.txt")).items()}

    runs = []
    for d in sorted(glob.glob(os.path.join(top, "[0-9][0-9]_*"))):
        name = os.path.basename(d).split("_", 1)[1]
        w = read(os.path.join(d, "wall_s.txt"))
        snap = None
        if os.path.exists(os.path.join(d, "snapshot.json")):
            try:
                with open(os.path.join(d, "snapshot.json")) as f: snap = json.load(f)
            except ValueError as e:
                bad(f"{os.path.basename(d)}: snapshot.json unreadable ({e})")
        runs.append({"dir": d, "id": os.path.basename(d), "level": name,
                     "wall": float(w) if w else None, "rsl": rsl(os.path.join(d, "rsl.error.0000")),
                     "md5": files(os.path.join(d, "wrfout_md5.txt")),
                     "size": {k: int(v) for k, v in files(os.path.join(d, "wrfout_size.txt")).items()},
                     "snap": snap})
    if not runs:
        print("no runs under " + top); return 2

    print("== WRF itself")
    for r in runs:
        x = r["rsl"]
        check(x["success"], f"{r['id']}: SUCCESS COMPLETE WRF")
        info(f"{r['id']}: wall {r['wall']:.1f} s, main {x['main_s']:.1f} s over {x['main_steps']} steps, "
             f"writing {x['write_s']:.1f} s over {x['writes']} files, input {x['input_s']:.1f} s"
             if r["wall"] else f"{r['id']}: no wall time")

    base = [r for r in runs if r["level"] == "off" and r["wall"] and r["rsl"]["success"]]
    prof = [r for r in runs if r["level"] != "off"]

    print("\n== overhead (wall time, profiled vs unprofiled)")
    if not base:
        bad("no successful unprofiled ('off') run to compare against")
    else:
        ref = min(r["wall"] for r in base)
        ref_main = min(r["rsl"]["main_s"] for r in base)
        noise = 0.0
        if len(base) > 1:
            noise = 100.0 * (max(r["wall"] for r in base) - ref) / ref
            info(f"unprofiled runs: {', '.join('%.1f' % r['wall'] for r in base)} s, spread {noise:.1f}%")
        for r in prof:
            if not r["wall"]: continue
            ov = 100.0 * (r["wall"] - ref) / ref
            ovm = 100.0 * (r["rsl"]["main_s"] - ref_main) / ref_main if ref_main else 0
            lim = OVERHEAD.get(r["level"], 5.0)
            msg = (f"{r['id']}: wall +{ov:.1f}% (WRF main loop {ovm:+.1f}%), "
                   f"limit {lim:.0f}% + noise {noise:.1f}%")
            check(ov <= lim + noise, msg)

    print("\n== results unchanged")
    if base and base[0]["md5"]:
        ref = base[0]["md5"]
        if len(base) > 1 and base[1]["md5"] != ref:
            warn("the two unprofiled runs differ from each other (non-reproducible build or "
                 "threading); output comparison skipped")
        else:
            for r in prof:
                if not r["md5"]: bad(f"{r['id']}: no output checksums"); continue
                diff = sorted(k for k in ref if r["md5"].get(k) != ref[k])
                check(not diff and set(r["md5"]) == set(ref),
                      f"{r['id']}: {len(ref)} output files byte-identical to the unprofiled run"
                      + (f" (differ: {', '.join(diff[:4])})" if diff else ""))
    else:
        warn("no output checksums from an unprofiled run")

    good = [r for r in prof if r["snap"]]
    for r in prof:
        if not r["snap"]: bad(f"{r['id']}: no snapshot.json written")

    for r in good:
        s, x, lvl = r["snap"], r["rsl"], r["level"]
        M = s.get("metrics", {})
        print(f"\n== {r['id']} (level {lvl})")
        check(g(s, "run", "level") == lvl, f"snapshot level {g(s, 'run', 'level')!r}")

        # shape
        nr = g(s, "run", "nranks")
        if x["ntasks"]:
            check(nr == x["ntasks"], f"ranks {nr} == WRF ntasks_x*ntasks_y {x['ntasks']}")
        info(f"{nr} ranks on {g(s, 'run', 'nnodes')} nodes, "
             f"{g(s, 'run', 'ranks_per_node', 'max')} per node, {g(s, 'run', 'cpus_per_rank', 'min')}-"
             f"{g(s, 'run', 'cpus_per_rank', 'max')} CPUs per rank, MPI thread level "
             f"{g(s, 'run', 'mpi_thread_level')}, {str(g(s, 'run', 'mpi_library')).split(',')[0]}")
        ot = g(s, "run", "omp_num_threads")
        try: ot = int(ot)
        except (TypeError, ValueError): ot = None
        if omp:
            check(ot == omp, f"omp_num_threads {ot} == OMP_NUM_THREADS {omp}")
        if x["tiles"] and ot:
            info(f"WRF tiles per patch {x['tiles']}, OpenMP threads {ot}")
        cpr = g(s, "run", "cpus_per_rank", "min")
        if omp and cpr is not None:
            check(cpr >= omp, f"each rank may run on >= {omp} CPUs (min {cpr}): "
                              "threads are not stacked on one core")
        omp_m = M.get("openmp", {})
        if omp_m.get("available"):
            check(omp_m.get("threads") == nr * (ot or 1),
                  f"OpenMP threads seen {omp_m.get('threads')} == ranks x threads {nr * (ot or 1)}")
            info(f"OpenMP parallel {omp_m.get('parallel_pct_of_elapsed')}% of elapsed, "
                 f"thread imbalance {omp_m.get('imbalance_pct')}%, runtime {omp_m.get('runtime')}")
        else:
            warn("OpenMP block unavailable (no OMPT in the runtime?)")

        # time
        el = g(s, "run", "elapsed_s", "max")
        # WRF's main-step and writing timers may overlap (output inside a
        # step), so each must fit in elapsed on its own, not their sum
        need = max(x["main_s"], x["write_s"], x["input_s"])
        check(el and r["wall"] and 0.98 * need <= el <= r["wall"],
              f"elapsed {el:.2f} s covers WRF's main loop {x['main_s']:.2f} s and fits in the "
              f"wall {r['wall']:.2f} s" if el else "elapsed present")
        ovf = g(s, "overhead", "fraction_of_elapsed")
        if ovf is not None:
            info(f"miniprof's own init+finalize {100 * ovf:.2f}% of elapsed")
        split = g(M, "parallel", "time_split_pct", default={})
        info("time split: compute %.1f%%  MPI %.1f%%  I/O %.1f%%" % tuple(
            split.get(k) or 0 for k in ("compute", "mpi", "io")))
        check(abs(sum(v or 0 for v in split.values()) - 100) < 1.5, "time split sums to 100%")

        # I/O
        io = g(M, "parallel", "io", default={})
        mw = g(io, "mpiio", "bytes", "write", default=0) or 0
        pw = g(io, "posix", "bytes", "write", default=0) or 0
        pr = g(io, "posix", "bytes", "read", default=0) or 0
        mr = g(io, "mpiio", "bytes", "read", default=0) or 0
        out_bytes = sum(r["size"].values())
        info(f"output files {out_bytes / 1e9:.2f} GB; written via MPI-IO {mw / 1e9:.2f} GB, "
             f"POSIX {pw / 1e9:.2f} GB; read MPI-IO {mr / 1e9:.2f} GB, POSIX {pr / 1e9:.2f} GB "
             f"(inputs {sum(inputs.values()) / 1e9:.2f} GB)")
        if out_bytes:
            # io_form 11 writes through PnetCDF -> MPI-IO; the header may be
            # rewritten, so allow a little more than the file sizes
            check(0.95 * out_bytes <= mw <= 1.5 * out_bytes,
                  f"MPI-IO bytes written {mw / out_bytes:.2f} x the output file sizes (0.95-1.5)")
            if pw > 0.5 * out_bytes:
                warn(f"POSIX writes {pw / 1e9:.2f} GB: also counted under MPI-IO? (would double count)")
        io_max = g(io, "time_s", "max", default=0) or 0
        if x["write_s"] > 0:
            ratio = io_max / x["write_s"]
            info(f"I/O time slowest rank {io_max:.1f} s; WRF Timing for Writing {x['write_s']:.1f} s "
                 f"(ratio {ratio:.2f}; WRF's figure also holds the gathers and netCDF bookkeeping)")
            check(0.2 <= ratio <= 1.3, "I/O time within 0.2-1.3x of WRF's writing time")

        # MPI
        mpi = g(M, "parallel", "mpi", default={})
        bc = mpi.get("by_class", {})
        p2p = g(bc, "p2p", "calls", default=0) or 0
        check(p2p > 0, f"halo exchange seen: {p2p} point-to-point calls")
        if x["main_steps"] and nr:
            per = p2p / nr / x["main_steps"]
            info(f"{per:.0f} p2p calls per rank per time step")
        info("top MPI: " + ", ".join("%s %.1f%%" % (f["name"], f["pct_of_elapsed"] or 0)
                                     for f in mpi.get("by_function", [])[:5]))
        check(0.5 <= (mpi.get("pct_of_elapsed") or 0) <= 60,
              "MPI %.1f%% of elapsed (plausible 0.5-60%%), imbalance %.1f%%"
              % (mpi.get("pct_of_elapsed") or 0, mpi.get("imbalance_pct") or 0))

        # counters
        c = M.get("counters", {})
        if c.get("available"):
            check(not c.get("ranks_without_counters"), f"counters on every rank ({c.get('uarch')}, "
                  f"group {c.get('group')}, read {c.get('read_method')})")
            check(not c.get("threads_dropped"), f"threads counted {c.get('threads_counted')}, "
                  f"dropped {c.get('threads_dropped')}")
            if c.get("multiplexed_ranks"):
                warn(f"{c['multiplexed_ranks']} ranks multiplexed (NMI watchdog or another perf user?)")
            d = c.get("derived", {})
            ipc = d.get("ipc_compute") or d.get("ipc")
            check(ipc and 0.2 <= ipc <= 5, f"IPC in compute {ipc}, GHz {d.get('ghz_effective')}, "
                  f"GFLOP/s {d.get('gflops_per_s')}")
            if lvl in ("basic", "roofline") and d.get("flops_in_mpi_pct") is not None:
                check(d["flops_in_mpi_pct"] < 2, f"FLOPs inside MPI {d['flops_in_mpi_pct']}% (should be ~0)")
        else:
            bad(f"counters unavailable: {c.get('reason')}")

        if lvl == "topdown":
            td = M.get("topdown", {})
            l1 = g(td, "compute", "level1", default={})
            if td.get("available"):
                tot = sum(v for v in l1.values() if isinstance(v, (int, float)))
                check(97 <= tot <= 103, "topdown level 1 sums to %.1f%%: %s" % (tot, ", ".join(
                    f"{k} {v}" for k, v in l1.items() if v is not None)))
            else:
                bad(f"topdown unavailable: {td.get('reason')}")

        mem = M.get("memory", {})
        if mem.get("available"):
            check(not mem.get("nodes_without"), f"memory measured on {mem.get('nodes_measured')} nodes "
                  f"({mem.get('nodes_without')} without)")
            info(f"DRAM {mem.get('total_gb_s_all_nodes')} GB/s all nodes, energy "
                 f"{mem.get('energy_j_all_nodes')} J, remote reads {mem.get('remote_read_share_pct')}%")
        else:
            warn(f"memory block unavailable: {mem.get('reason')} "
                 "(setcap on every node, or perf_event_paranoid <= 0)")

        if lvl == "roofline":
            rf = M.get("roofline", {})
            if rf.get("available"):
                n0 = (rf.get("nodes") or [{}])[0]
                info(f"roofline node 0: {n0.get('gflops_per_s')} GFLOP/s, {n0.get('dram_gb_s')} GB/s, "
                     f"AI {n0.get('arithmetic_intensity')}, {n0.get('bound')} bound, "
                     f"{n0.get('pct_of_attainable')}% of attainable")
                check(0 < (n0.get("pct_of_attainable") or 0) <= 110,
                      "roofline point under its roof (<= 110% of attainable)")
                regs = rf.get("regions") or []
                info(f"{len(regs)} roofline regions" if regs else f"no per-region roofline: {rf.get('regions_reason')}")
            elif rf.get("estimate"):
                warn(f"roofline is an estimate only: {rf.get('reason')}")
            else:
                bad(f"roofline unavailable: {rf.get('reason')}")

        # tools
        for f in ("report.txt", "report.html", "csv"):
            check(os.path.exists(os.path.join(r["dir"], f)), f"{f} written")

    print("\n== miniprof_diff at scale")
    if len(good) >= 2:
        a, b = good[0], good[-1]
        out = os.path.join(top, f"diff_{a['level']}_{b['level']}.html")
        p = subprocess.run([sys.executable, os.path.join(HERE, "miniprof_diff.py"),
                            os.path.join(a["dir"], "snapshot.json"), os.path.join(b["dir"], "snapshot.json"),
                            "--label-a", a["level"], "--label-b", b["level"], "--html", out],
                           capture_output=True, text=True)
        check(p.returncode == 0 and os.path.exists(out), f"diff {a['level']} vs {b['level']} -> {out}")
        for line in p.stdout.splitlines()[:6]: info(line)
        ea, eb = g(a["snap"], "run", "elapsed_s", "max"), g(b["snap"], "run", "elapsed_s", "max")
        if ea and eb:
            info(f"same application at two levels: elapsed ratio {eb / ea:.3f} "
                 "(differences are profiling overhead plus run-to-run noise)")
    else:
        warn("fewer than two snapshots: diff not exercised")

    print()
    if fails:
        print(f"{len(fails)} CHECK(S) FAILED"); return 1
    print("ALL WRF CHECKS PASSED"); return 0


if __name__ == "__main__":
    sys.exit(main())
