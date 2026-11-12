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
import os, re, sys
from appcheck import (Report, read, files, g, env_omp, load_runs, check_overhead, check_identical,
                      check_shape, time_split, mpi_summary, check_hardware, check_diff)


def rsl(path):
    """WRF's own timings and decomposition from rsl.error.0000."""
    t = read(path, "") or ""
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


def main():
    if len(sys.argv) != 2:
        print(__doc__); return 2
    top, rep = sys.argv[1], Report()
    omp = env_omp(top)
    inputs = {k: int(v) for k, v in files(os.path.join(top, "input_size.txt")).items()}
    runs = load_runs(top, rep, md5="wrfout_md5.txt", size="wrfout_size.txt")
    if not runs:
        print("no runs under " + top); return 2
    for r in runs:
        r["rsl"] = rsl(os.path.join(r["dir"], "rsl.error.0000"))

    rep.section("WRF itself")
    for r in runs:
        x = r["rsl"]
        rep.check(x["success"], f"{r['id']}: SUCCESS COMPLETE WRF")
        if r["wall"]:
            rep.info(f"{r['id']}: wall {r['wall']:.1f} s, main {x['main_s']:.1f} s over {x['main_steps']} steps, "
                     f"writing {x['write_s']:.1f} s over {x['writes']} files, input {x['input_s']:.1f} s")

    base = [r for r in runs if r["level"] == "off" and r["wall"] and r["rsl"]["success"]]
    prof = [r for r in runs if r["level"] != "off"]
    check_overhead(rep, base, prof, inner=lambda r: r["rsl"]["main_s"], inner_name="WRF main loop", runs=runs)
    check_identical(rep, base, prof, lambda r: r["md5"], "output files")

    good = [r for r in prof if r["snap"]]
    for r in prof:
        if not r["snap"]: rep.bad(f"{r['id']}: no snapshot.json written")

    for r in good:
        s, x, lvl = r["snap"], r["rsl"], r["level"]
        M = s.get("metrics", {})
        rep.section(f"{r['id']} (level {lvl})")
        rep.check(g(s, "run", "level") == lvl, f"snapshot level {g(s, 'run', 'level')!r}")
        nr, ot = check_shape(rep, s, omp, ranks=x["ntasks"])
        if x["tiles"] and ot:
            rep.info(f"WRF tiles per patch {x['tiles']}, OpenMP threads {ot}")

        # WRF's main-step and writing timers may overlap (output inside a
        # step), so each must fit in elapsed on its own, not their sum
        el = g(s, "run", "elapsed_s", "max")
        need = max(x["main_s"], x["write_s"], x["input_s"])
        rep.check(bool(el and r["wall"]) and 0.98 * need <= el <= r["wall"],
                  f"elapsed {el:.2f} s covers WRF's main loop {x['main_s']:.2f} s and fits in the "
                  f"wall {r['wall']:.2f} s" if el and r["wall"] else "elapsed present")
        ovf = g(s, "overhead", "fraction_of_elapsed")
        if ovf is not None:
            rep.info(f"miniprof's own init+finalize {100 * ovf:.2f}% of elapsed")
        time_split(rep, s)

        io = g(M, "parallel", "io", default={}) or {}
        mw = g(io, "mpiio", "bytes", "write", default=0) or 0
        pw = g(io, "posix", "bytes", "write", default=0) or 0
        pr = g(io, "posix", "bytes", "read", default=0) or 0
        mr = g(io, "mpiio", "bytes", "read", default=0) or 0
        out_bytes = sum(r["size"].values())
        rep.info(f"output files {out_bytes / 1e9:.2f} GB; written via MPI-IO {mw / 1e9:.2f} GB, "
                 f"POSIX {pw / 1e9:.2f} GB; read MPI-IO {mr / 1e9:.2f} GB, POSIX {pr / 1e9:.2f} GB "
                 f"(inputs {sum(inputs.values()) / 1e9:.2f} GB)")
        if out_bytes:
            # io_form 11 writes through PnetCDF -> MPI-IO; the header may be
            # rewritten, so allow a little more than the file sizes
            rep.check(0.95 * out_bytes <= mw <= 1.5 * out_bytes,
                      f"MPI-IO bytes written {mw / out_bytes:.2f} x the output file sizes (0.95-1.5)")
            if pw > 0.5 * out_bytes:
                rep.warn(f"POSIX writes {pw / 1e9:.2f} GB: also counted under MPI-IO? (would double count)")
        io_max = g(io, "time_s", "max", default=0) or 0
        if x["write_s"] > 0:
            ratio = io_max / x["write_s"]
            rep.info(f"I/O time slowest rank {io_max:.1f} s; WRF Timing for Writing {x['write_s']:.1f} s "
                     f"(ratio {ratio:.2f}; WRF's figure also holds the gathers and netCDF bookkeeping)")
            rep.check(0.2 <= ratio <= 1.3, "I/O time within 0.2-1.3x of WRF's writing time")

        mpi_summary(rep, s, steps=x["main_steps"])
        check_hardware(rep, r)

    check_diff(rep, top, good)
    return rep.finish("WRF")


if __name__ == "__main__":
    sys.exit(main())
