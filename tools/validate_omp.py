#!/usr/bin/env python3
"""
validate_omp.py — check a snapshot of examples/omp_validate against the
analytic answer (see the header of omp_validate.c).

Exit criteria for phase P2 (T threads per rank, R ranks):
  - the OpenMP block is available and every region has its own name;
    the first region (warmup_phase) carries the runtime's thread start-up
    and is reported, not checked;
  - every region ran with a team of T threads;
  - per-region wall time per rank matches the injected spin times (5%);
  - per-region imbalance (barrier wait / team time) within 3 points of:
        balanced            0
        imbalanced, loop    (T-1) / (2T)
  - serial time >= 95% of the injected 200 ms;
  - lock wait in the critical phase ~ 40 T (T-1) ms per rank (> 70%).

Exit 77 when OpenMP was not measured (GCC libgomp has no OMPT): the make
target reports SKIP, not a failure.

Usage: validate_omp.py SNAPSHOT.json --threads T
"""

import argparse
import json
import sys

SERIAL_MS, BAL_MS, BASE_MS, DELTA_MS = 200.0, 100.0, 50.0, 50.0
CRIT_MS, CRIT_ITERS, LOOP_MS, WARM_MS = 2.0, 20, 5.0, 20.0
REL = 0.05          # wall-time tolerance
ABS_MS = 3.0        # ... with a floor
IMB_PTS = 3.0       # imbalance tolerance, percentage points


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--threads", type=int, required=True)
    a = ap.parse_args()
    T = a.threads

    omp = json.load(open(a.file))["metrics"].get("openmp", {})
    if not omp.get("available"):
        print(f"SKIP: OpenMP not measured: {omp.get('reason', 'no openmp block')}")
        return 77

    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    R = omp["threads"] // T if T else 0
    print(f"runtime: {omp.get('runtime')}  ranks={R}  threads/rank={T}")
    check(omp["threads"] == R * T and R > 0,
          f"threads {omp['threads']} = ranks x {T}")
    check(omp.get("ranks_without_ompt", 0) == 0, "every rank measured")

    regions = {r["name"]: r for r in omp["regions"]}
    imb = 100.0 * (T - 1) / (2 * T)
    expect = {   # name: (wall ms per rank, imbalance %, check imbalance?)
        "balanced_phase":   (BAL_MS, 0.0, True),
        "imbalanced_phase": (BASE_MS + (T - 1) * DELTA_MS, imb, True),
        "critical_phase":   (T * CRIT_ITERS * CRIT_MS, None, False),
        "loop_phase":       (4 * LOOP_MS * T, imb, True),
    }
    w = regions.get("warmup_phase")
    check(w is not None, "region warmup_phase present")
    if w:
        print(f"  info  warmup_phase (first region, includes thread start-up): "
              f"wall {w['wall_s_per_rank'] * 1e3:.1f} ms/rank vs {WARM_MS:.0f} spun, "
              f"imbalance {w['imbalance_pct']:.1f}% (not checked)")
    for name, (wall, eimb, chk) in expect.items():
        r = regions.get(name)
        if r is None:
            check(False, f"region {name} present (got {sorted(regions)})")
            continue
        got = r["wall_s_per_rank"] * 1e3
        check(abs(got - wall) <= max(REL * wall, ABS_MS),
              f"{name}: wall {got:.1f} ms/rank vs {wall:.1f}")
        check(abs(r["avg_team"] - T) < 1e-9, f"{name}: team {r['avg_team']:g} = {T}")
        check(r["ranks"] == R and r["calls"] == R, f"{name}: ranks/calls = {R}")
        if chk:
            check(abs(r["imbalance_pct"] - eimb) <= IMB_PTS,
                  f"{name}: imbalance {r['imbalance_pct']:.1f}% vs {eimb:.1f}%")

    ser = omp["serial_s"]["min"] * 1e3
    check(ser >= 0.95 * SERIAL_MS, f"serial {ser:.1f} ms >= 95% of {SERIAL_MS:.0f}")

    lock = omp["thread_time_s"]["lock_wait"] * 1e3 / R
    elock = CRIT_ITERS * CRIT_MS * T * (T - 1)
    if T > 1:
        check(0.7 * elock <= lock <= 1.1 * elock,
              f"lock wait {lock:.1f} ms/rank vs ~{elock:.0f}")
    else:
        check(lock < 1.0, f"lock wait {lock:.2f} ms/rank ~ 0 with one thread")

    if any("location" not in r for r in regions.values()):
        print("  note  regions without file:line: is llvm-addr2line on PATH? "
              "(old GNU addr2line cannot read clang's DWARF 5; see MINIPROF_ADDR2LINE)")
    print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
