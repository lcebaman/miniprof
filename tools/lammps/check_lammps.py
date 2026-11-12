#!/usr/bin/env python3
"""check_lammps.py — check miniprof snapshots of LAMMPS against LAMMPS itself.

Reads the directory written by tools/lammps/validate_lammps.sh and checks:
  overhead   wall time against the unprofiled runs (and their own spread)
  results    thermo output and trajectory dumps identical to the unprofiled run
  shape      ranks and threads against LAMMPS's "M MPI tasks x T OpenMP threads"
  time       elapsed covers LAMMPS's loop time; compute time holds Pair + Bond +
             Neigh; MPI time between LAMMPS's Comm and its communicating sections
  I/O        bytes written against the dump and log sizes; I/O time against Output
  MPI        halo exchange and collectives present
  counters   no dropped threads, IPC and topdown sane, memory and roofline
  tools      report, HTML, CSV written; miniprof_diff runs on two levels
Exit 0 = all checks passed (WARN lines do not fail).
"""
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from appcheck import (Report, read, files, g, env_omp, load_runs, check_overhead, check_identical,
                      check_shape, time_split, mpi_summary, check_hardware, check_diff)

SECTIONS = ("Pair", "Bond", "Kspace", "Neigh", "Comm", "Output", "Modify", "Other")


def lammps_log(path):
    t = read(path, "") or ""
    r = {"done": "Total wall time:" in t, "error": None}
    m = re.search(r"^ERROR.*$", t, re.M)
    if m: r["error"] = m.group(0)
    m = re.search(r"Loop time of ([0-9.eE+-]+) on (\d+) procs for (\d+) steps with (\d+) atoms", t)
    r["loop_s"], r["procs"], r["steps"], r["atoms"] = (
        (float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else (None,) * 4)
    m = re.search(r"(\d+) MPI tasks x (\d+) OpenMP threads", t)
    r["ranks"], r["threads"] = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    m = re.search(r"Performance: .*", t)
    r["performance"] = m.group(0) if m else ""
    # MPI task timing breakdown: Section | min | avg | max | %varavg | %total
    r["sec"] = {}
    for name in SECTIONS:
        m = re.search(rf"^{name}\s*\|\s*([0-9.eE+-]*)\s*\|\s*([0-9.eE+-]+)\s*\|\s*([0-9.eE+-]*)\s*\|", t, re.M)
        if m:
            f = lambda x: float(x) if x else None
            r["sec"][name] = {"min": f(m.group(1)), "avg": f(m.group(2)), "max": f(m.group(3))}
    # thermo blocks, without the CPU time in their header line
    r["thermo"] = {}
    for m in re.finditer(r"-+ Step\s+(\d+) -+ CPU = [^\n]*\n((?:(?!-+ Step|Loop time)[^\n]*\n)+)", t):
        r["thermo"][f"step {m.group(1)}"] = " ".join(m.group(2).split())
    r["log_bytes"] = len(t.encode())
    return r


def sec(x, name, k="avg"):
    return (x["sec"].get(name) or {}).get(k) or 0.0


def main():
    if len(sys.argv) != 2:
        print(__doc__); return 2
    top, rep = sys.argv[1], Report()
    omp = env_omp(top)
    runs = load_runs(top, rep)
    data_bytes = sum(int(v) for v in files(os.path.join(top, "input_size.txt")).values())
    if not runs:
        print("no runs under " + top); return 2
    for r in runs:
        r["lmp"] = lammps_log(os.path.join(r["dir"], "log.lammps"))
        r["stdout_bytes"] = os.path.getsize(os.path.join(r["dir"], "stdout.txt")) \
            if os.path.exists(os.path.join(r["dir"], "stdout.txt")) else 0

    rep.section("LAMMPS itself")
    for r in runs:
        x = r["lmp"]
        rep.check(x["done"] and not x["error"] and x["loop_s"],
                  f"{r['id']}: run completed" + (f" ({x['error']})" if x["error"] else ""))
        if x["loop_s"] and r["wall"]:
            rep.info(f"{r['id']}: wall {r['wall']:.1f} s, loop {x['loop_s']:.1f} s, {x['steps']} steps, "
                     f"{x['atoms']} atoms, {x['ranks']} ranks x {x['threads']} threads; "
                     + "  ".join(f"{n} {sec(x, n):.1f}" for n in SECTIONS))

    base = [r for r in runs if r["level"] == "off" and r["wall"] and r["lmp"]["loop_s"]]
    prof = [r for r in runs if r["level"] != "off"]
    check_overhead(rep, base, prof, inner=lambda r: r["lmp"]["loop_s"] or 0, inner_name="LAMMPS loop", runs=runs)
    check_identical(rep, base, prof, lambda r: r["lmp"]["thermo"], "thermo outputs")
    check_identical(rep, base, prof, lambda r: r["md5"], "trajectory dumps")

    good = [r for r in prof if r["snap"]]
    for r in prof:
        if not r["snap"]: rep.bad(f"{r['id']}: no snapshot.json written")

    for r in good:
        s, x, lvl = r["snap"], r["lmp"], r["level"]
        M = s.get("metrics", {})
        rep.section(f"{r['id']} (level {lvl})")
        rep.check(g(s, "run", "level") == lvl, f"snapshot level {g(s, 'run', 'level')!r}")
        check_shape(rep, s, omp, ranks=x["ranks"], threads=x["threads"])

        el = g(s, "run", "elapsed_s", "avg") or 0
        el_max = g(s, "run", "elapsed_s", "max") or 0
        loop = x["loop_s"] or 0
        rep.check(el_max and r["wall"] and 0.98 * loop <= el_max <= r["wall"],
                  f"elapsed {el_max:.2f} s covers LAMMPS's loop {loop:.2f} s and fits in the wall {r['wall']:.2f} s")
        setup = max(el - loop, 0.0)        # read_data, replicate, setup: outside the loop timers
        time_split(rep, s)

        mpi_avg = g(M, "parallel", "mpi", "time_s", "avg", default=0) or 0
        io = g(M, "parallel", "io", default={}) or {}
        io_avg = g(io, "time_s", "avg", default=0) or 0
        compute = el - mpi_avg - io_avg
        busy = sec(x, "Pair") + sec(x, "Bond") + sec(x, "Neigh")
        rep.info(f"per rank (avg): elapsed {el:.2f} s = compute {compute:.2f} + MPI {mpi_avg:.2f} + "
                 f"I/O {io_avg:.2f}; LAMMPS: Pair+Bond+Neigh {busy:.2f}, Comm {sec(x, 'Comm'):.2f}, "
                 f"Kspace {sec(x, 'Kspace'):.2f}, Modify {sec(x, 'Modify'):.2f}, Output {sec(x, 'Output'):.2f}, "
                 f"Other {sec(x, 'Other'):.2f}, outside the loop {setup:.2f}")
        # Pair, Bond and Neigh make no MPI calls (neighbor-list builds call
        # Comm, which LAMMPS times as Comm): miniprof's compute must hold them
        rep.check(compute >= 0.95 * busy,
                  f"compute {compute:.2f} s >= 0.95 x LAMMPS's Pair+Bond+Neigh {busy:.2f} s "
                  "(MPI time not taken from compute)")
        # MPI happens in Comm (halo exchange, migration) and inside Kspace
        # (FFT transposes), Modify (SHAKE, NPT reductions), Output (gathers),
        # Other (load-imbalance waits) and setup; Comm also packs buffers
        hi = sum(sec(x, n) for n in ("Comm", "Kspace", "Modify", "Output", "Other")) + setup + 0.01 * el
        lo = 0.3 * sec(x, "Comm")
        multi = (g(s, "run", "nranks") or 1) > 1
        if multi:
            rep.check(lo <= mpi_avg <= hi, f"MPI {mpi_avg:.2f} s within [0.3 x Comm {lo:.2f}, "
                      f"communicating sections + setup {hi:.2f}] s")

        # I/O: rank 0 writes the dumps and the log; it also reads data.rhodo
        pw = g(io, "posix", "bytes", "write", default=0) or 0
        mw = g(io, "mpiio", "bytes", "write", default=0) or 0
        pr = g(io, "posix", "bytes", "read", default=0) or 0
        dumps = sum(r["size"].values())
        rep.info(f"dumps {dumps / 1e6:.1f} MB, log {x['log_bytes'] / 1e6:.2f} MB; written POSIX "
                 f"{pw / 1e6:.1f} MB, MPI-IO {mw / 1e6:.1f} MB; read POSIX {pr / 1e6:.1f} MB")
        if dumps:
            hi_b = 1.1 * (dumps + x["log_bytes"] + r["stdout_bytes"]) + 1e6
            rep.check(dumps <= pw + mw <= hi_b,
                      f"bytes written {(pw + mw) / 1e6:.1f} MB within [dumps {dumps / 1e6:.1f}, "
                      f"dumps + log + stdout {hi_b / 1e6:.1f}] MB")
        if data_bytes:
            # read_data reads atoms and velocities once and scans, then reads,
            # each topology section: between 1x and 2x the data file
            rep.check(data_bytes <= pr <= 2.05 * data_bytes,
                      f"bytes read {pr / 1e6:.1f} MB within [1, 2.05] x data.rhodo {data_bytes / 1e6:.1f} MB "
                      "(read_data scans the topology sections, then reads them)")
        io_max = g(io, "time_s", "max", default=0) or 0
        out_max = sec(x, "Output", "max") or sec(x, "Output")
        rep.check(io_max <= 1.2 * out_max + setup + 0.2,
                  f"I/O time slowest rank {io_max:.2f} s <= 1.2 x LAMMPS Output {out_max:.2f} s + setup "
                  f"{setup:.2f} s (Output also holds the gather to rank 0)")

        mpi = mpi_summary(rep, s, steps=x["steps"])
        coll = g(mpi, "by_class", "collective", "calls", default=0) or 0
        if multi:
            rep.check(coll > 0, f"collectives seen: {coll} calls (thermo, NPT and SHAKE reductions)")
        check_hardware(rep, r)

    check_diff(rep, top, good)
    return rep.finish("LAMMPS")


if __name__ == "__main__":
    sys.exit(main())
