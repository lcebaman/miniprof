#!/usr/bin/env python3
"""
validate_counters.py — check a snapshot of examples/counters_validate.

Checks (see the header of counters_validate.c for the expected values):
  base group (MINIPROF_SNAPSHOT=basic):
    - region FLOPs exact to 0.5%: daxpy, dot_scalar, outer, inner, thread_work
    - nesting: outer == 2 x inner
    - worker threads counted (thread_work region and run totals)
    - FLOPs inside MPI ~ 0; IPC, effective GHz plausible
    - run total FLOPs within the known range (regions + init + checksum)
    - with --perf-flops: total within 1% of `perf stat` on the same binary
  vector group (MINIPROF_SNAPSHOT=roofline):
    - dot_scalar: scalar ops = N*REPS (FMA) or 2*N*REPS (exact arithmetic)
    - daxpy: packed ops dominate (vectorised)
    (width events include FP loads/stores/moves, so no exact packed counts)
Exit 0 on pass, 1 on failure, 77 (skip) if counters are unavailable.

Usage:
  validate_counters.py SNAPSHOT.json --n N --reps REPS [--perf-flops TOTAL]
"""

import argparse
import json
import sys

TOL = 0.005


def plumbing(c, nranks) -> int:
    """MINIPROF_COUNTERS_TEST=1: software events (task-clock as 'cycles').
    Exercises the engine without a PMU; checks structure, not hardware."""
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print(("PASS: " if ok else "FAIL: ") + msg)
        fails += 0 if ok else 1

    check(c["threads_counted"] == 3 * nranks,
          f"threads counted {c['threads_counted']} == {3 * nranks} (main + 2 pthreads per rank)")
    check(c["threads_dropped"] == 0, "no threads dropped")
    check(c["reads"]["syscall"] + c["reads"]["rdpmc"] > 0, f"boundary reads happened ({c['reads']})")
    t, cp, mp = c["totals"]["cycles"], c["compute"]["cycles"], c["mpi"]["cycles"]
    check(t == cp + mp, f"compute + mpi == totals ({cp} + {mp} == {t})")
    check(mp > 0, "time inside MPI attributed to the mpi bucket")
    reg = {r["name"]: r for r in c["regions"]}
    for name in ("daxpy", "dot_scalar", "outer", "inner"):
        r = reg.get(name)
        if not r:
            check(False, f"region {name!r} present")
            continue
        wall_ns = r["time_s"]["avg"] * r["ranks"] * 1e9
        cpu_ns = r["totals"]["cycles"]
        # single-threaded CPU-bound region: task-clock ~ wall (less if descheduled)
        check(0.85 * wall_ns <= cpu_ns <= 1.01 * wall_ns,
              f"region {name}: task-clock {cpu_ns / 1e6:.1f} ms vs wall {wall_ns / 1e6:.1f} ms")
    if "outer" in reg and "inner" in reg:
        check(reg["outer"]["totals"]["cycles"] > reg["inner"]["totals"]["cycles"],
              "nested: outer > inner")
    tw = reg.get("thread_work")
    check(tw is not None and tw["calls"] == 2 * nranks and tw["totals"]["cycles"] > 0,
          "thread_work region counted on worker threads")
    print(f"\n{'ALL COUNTER PLUMBING CHECKS PASSED' if fails == 0 else f'{fails} PLUMBING CHECK(S) FAILED'}")
    return 0 if fails == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--reps", type=int, required=True)
    ap.add_argument("--perf-flops", type=float, default=None,
                    help="total FLOPs from perf stat (user space), summed over ranks")
    a = ap.parse_args()

    doc = json.load(open(a.file))
    c = doc["metrics"]["counters"]
    nranks = doc["run"]["nranks"]
    if not c.get("available"):
        print(f"SKIP: counters unavailable: {c.get('reason')}")
        return 77

    fails = 0
    if c.get("uarch") == "swtest":
        return plumbing(c, nranks)

    def check(ok, msg):
        nonlocal fails
        print(("PASS: " if ok else "FAIL: ") + msg)
        fails += 0 if ok else 1

    def near(got, want, tol=TOL):
        return want > 0 and abs(got - want) <= tol * want

    N, R = a.n, a.reps
    group = c["group"]
    print(f"INFO: uarch={c['uarch']} group={group} read_method={c['read_method']} "
          f"threads_counted={c['threads_counted']} multiplexed_ranks={c['multiplexed_ranks']}")
    print("INFO: events: " + ", ".join(f"{e['key']}={e['perf_name']}({e['config']})"
                                       + ("" if e["active"] else " [inactive]") for e in c["events"]))
    check(c["multiplexed_ranks"] == 0, "no rank was multiplexed")
    check(c["threads_counted"] >= 3 * nranks,
          f"threads counted {c['threads_counted']} >= {3 * nranks} (main + 2 workers per rank)")

    regions = {r["name"]: r for r in c["regions"]}
    for name in ("daxpy", "dot_scalar", "outer", "inner", "thread_work"):
        check(name in regions, f"region {name!r} present")
    if fails:
        return 1
    active = {e["key"] for e in c["events"] if e["active"]}
    dropped = [e["key"] for e in c["events"] if not e["active"]]
    if dropped:
        print(f"INFO: optional events dropped to fit the PMU: {', '.join(dropped)}")
    has_flops = "flops" in active

    def flops(name):
        return regions[name]["totals"].get("flops", 0)

    want = {
        "daxpy": 2 * N * R * nranks,
        "dot_scalar": 2 * N * R * nranks,
        "outer": 2 * N * R * nranks,
        "inner": 2 * N * (R // 2) * nranks,
        "thread_work": 2 * 2 * N * R * nranks,
    }
    check(regions["thread_work"]["calls"] == 2 * nranks,
          f"thread_work calls {regions['thread_work']['calls']} == {2 * nranks}")
    d = c["derived"]
    if has_flops:
        for name, w in want.items():
            got = flops(name)
            check(near(got, w), f"region {name}: flops {got} vs exact {w} ({100 * (got - w) / w:+.3f}%)")
        tot = c["totals"]["flops"]
        lo = sum(want[k] for k in ("daxpy", "dot_scalar", "outer", "thread_work"))
        hi = lo + nranks * 8 * N           # init (6 arrays x N) + checksum (N), generous
        check(lo * (1 - TOL) <= tot <= hi, f"run total flops {tot} within [{lo}, {hi}]")
        if a.perf_flops is not None:
            check(near(tot, a.perf_flops, 0.01),
                  f"run total flops {tot} vs perf stat {a.perf_flops:.0f} "
                  f"({100 * (tot - a.perf_flops) / a.perf_flops:+.3f}%, within 1%)")
        check(d.get("flops_in_mpi_pct", 100) < 0.1,
              f"FLOPs inside MPI {d.get('flops_in_mpi_pct'):.4f}% < 0.1%")

    if group == "base":
        check(0.05 < d.get("ipc", 0) < 8, f"IPC {d.get('ipc'):.3f} plausible")
        check(0.05 < d.get("ipc_compute", 0) < 8, f"IPC (compute) {d.get('ipc_compute'):.3f} plausible")
        ghz = d.get("ghz_effective", 0)
        check(0.5 < ghz < 7.0, f"effective frequency {ghz:.2f} GHz plausible")
        if "remote_dram_fill_pct" in d:
            print(f"INFO: dram_fill_gb_per_s={d.get('dram_fill_gb_per_s'):.2f} "
                  f"remote_dram_fill_pct={d['remote_dram_fill_pct']:.2f} "
                  f"l2_mpki={d.get('l2_misses_per_kilo_instr', 0):.2f}")
    elif group == "vector":
        # fp_ops_retired_by_width counts every FP-unit uop of that width,
        # INCLUDING loads, stores and register moves — not just arithmetic
        # (measured on EPYC 9555: SSE2 daxpy retires ~5 packed-128 uops per
        # 2 elements; the scalar dot product retires one packed move per
        # element). So only arithmetic-independent properties are checked.
        dt = regions["dot_scalar"]["totals"]
        scalar = dt.get("fp_scalar_ops", 0)
        ok = near(scalar, N * R * nranks) or near(scalar, 2 * N * R * nranks)
        check(ok, f"dot_scalar scalar ops {scalar} == N*REPS (FMA) or 2*N*REPS per rank")
        dx = regions["daxpy"]["totals"]
        packed = dx.get("fp_packed128_ops", 0) + dx.get("fp_packed256_ops", 0) + dx.get("fp_packed512_ops", 0)
        check(packed > 100 * max(dx.get("fp_scalar_ops", 0), 1),
              f"daxpy is vectorised: packed ops {packed} >> scalar ops {dx.get('fp_scalar_ops', 0)}")
        print("INFO: daxpy FP ops by width: " + ", ".join(
            f"{k}={dx.get(k, 0)}" for k in ("fp_scalar_ops", "fp_packed128_ops",
                                            "fp_packed256_ops", "fp_packed512_ops")))
        print(f"INFO: packed_fp_ops_pct={d.get('packed_fp_ops_pct', 0):.1f} "
              "(FP-unit ops incl. data movement, not arithmetic only)")

    print(f"\n{'ALL COUNTER CHECKS PASSED' if fails == 0 else f'{fails} COUNTER CHECK(S) FAILED'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
