#!/usr/bin/env python3
"""
validate_timeline.py — check the timeline block of a snapshot of
examples/timeline_validate (MINIPROF_TIMELINE=1, >= 2 ranks) against its
three phases of PHASE seconds:

  A  [0, P)      compute only: MPI avg < 10% in every bucket
  B  [P, 2P)     rank 0 slow: heatmap row of rank 0 < 35% MPI, the row of
                 the last rank > 50%
  C  [2P, 3P)    last rank slow: the reverse
Buckets within one bucket width (plus 10% of a phase) of a phase edge
are skipped: ranks do not start at exactly the same instant.

Also: the series have one value per bucket, t_s rises by bucket_s, and
min <= avg <= max.

Usage: validate_timeline.py SNAPSHOT.json [--phase SECONDS]
"""
import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--phase", type=float, default=0.6)
    a = ap.parse_args()
    t = (json.load(open(a.file)).get("metrics") or {}).get("timeline")
    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    check(t is not None and t.get("available"), "timeline block present")
    if fails:
        print("FAIL")
        return 1
    nb, w, P = t["buckets"], t["bucket_s"], a.phase
    check(len(t["t_s"]) == nb and all(len(t["mpi_pct"][k]) == nb for k in ("min", "avg", "max")),
          f"{nb} buckets of {w:g} s in every series")
    check(all(abs(t["t_s"][i] - i * w) < 1e-3 for i in range(nb)), "t_s rises by bucket_s")
    mm = t["mpi_pct"]
    check(all(x is None or (mm["min"][i] - 1e-6 <= x <= mm["max"][i] + 1e-6)
              for i, x in enumerate(mm["avg"])), "min <= avg <= max")
    check(nb * w >= 3 * P * 0.95, f"timeline covers the run ({nb * w:.2f} s >= {3 * P:.2f} s)")
    rows = t["heatmap"]["values"]
    last = len(rows) - 1
    check(last >= 1, f"heatmap has a row per rank group ({len(rows)} rows)")

    margin = w + 0.1 * P

    def buckets(lo, hi):
        return [i for i in range(nb) if lo + margin <= t["t_s"][i] and t["t_s"][i] + w <= hi - margin]

    A, B, C = buckets(0, P), buckets(P, 2 * P), buckets(2 * P, 3 * P)
    check(A and B and C, f"buckets inside each phase: A {len(A)}, B {len(B)}, C {len(C)}")

    def avg(xs):
        xs = [x for x in xs if x is not None]
        return sum(xs) / len(xs) if xs else float("nan")

    if A:
        a_mpi = max(mm["avg"][i] or 0 for i in A)
        check(a_mpi < 10, f"A (compute only): MPI avg <= {a_mpi:.1f}% (< 10)")
    if B and last >= 1:
        r0, rl = avg(rows[0][i] for i in B), avg(rows[last][i] for i in B)
        check(r0 < 35 and rl > 50, f"B (rank 0 slow): rank 0 MPI {r0:.0f}% (< 35), last rank {rl:.0f}% (> 50)")
    if C and last >= 1:
        r0, rl = avg(rows[0][i] for i in C), avg(rows[last][i] for i in C)
        check(rl < 35 and r0 > 50, f"C (last rank slow): last rank MPI {rl:.0f}% (< 35), rank 0 {r0:.0f}% (> 50)")
    if t.get("ipc"):
        ipc = [x for x in t["ipc"]["avg"] if x is not None]
        check(len(ipc) >= nb // 2, f"IPC in {len(ipc)} of {nb} buckets")

    # Node series (memory helper): on the ranks' buckets, and free of the
    # sampling alias (empty buckets, doubled ones, a spike at the end).
    # This program keeps every core busy (compute or MPI polling), so
    # package power is steady through all three phases.
    run_mem = ((json.load(open(a.file)).get("metrics") or {}).get("memory") or {})
    for m in (t.get("memory") or [])[:1]:
        check(abs(m["bucket_s"] - w) < 1e-9 and all(len(m[k]) == nb for k in ("read_gb_s", "write_gb_s", "power_w")),
              f"node series on the ranks' {nb} buckets")
        pw = [x for x in m["power_w"] if x is not None]
        if len(pw) >= 8 and sorted(pw)[len(pw) // 2] > 1:
            med = sorted(pw)[len(pw) // 2]
            lo, hi = min(pw[1:]), max(pw[1:])
            check(0.6 * med <= lo and hi <= 1.4 * med,
                  f"power steady: {lo:.0f}..{hi:.0f} W around a median of {med:.0f} W (within 40%)")
            tot = ((run_mem.get("per_node") or {}).get("power_w_avg"))
            if tot:
                check(abs(avg(pw) / tot - 1) < 0.15,
                      f"power series average {avg(pw):.0f} W vs run total {tot:.0f} W (within 15%)")

    print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
