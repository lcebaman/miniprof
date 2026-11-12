#!/usr/bin/env python3
"""
validate_imbalance.py — check a snapshot of examples/imbalance_validate
against the analytic answer (see the header of imbalance_validate.c).

Exit criteria for phase P1:
  - per-rank compute time and the compute imbalance match the injected
    spin times within 2% (relative / percentage points);
  - Allreduce wait time matches the injected imbalance within 2%;
  - MPI time agrees with trace mode's Table 2 within 2% (--trace-report);
  - I/O byte and call counts are exact.

Usage:
  validate_imbalance.py SNAPSHOT.json --steps 40 --base-ms 10 --delta-ms 10
                        [--trace-report miniprof_trace_report.txt]
"""

import argparse
import json
import re
import sys

KIB = 1024
POSIX_BYTES = 1536 * KIB      # 1 MiB raw + 512 KiB stdio, each direction
MPIIO_BYTES = 256 * KIB
REL = 0.02                    # 2 %
ABS_S = 0.005                 # 5 ms floor for timing comparisons


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--steps", type=int, required=True)
    ap.add_argument("--base-ms", type=float, required=True)
    ap.add_argument("--delta-ms", type=float, required=True)
    ap.add_argument("--trace-report")
    a = ap.parse_args()

    doc = json.load(open(a.file))
    par = doc["metrics"]["parallel"]
    ranks = doc["ranks"]
    n = len(ranks)
    fails = 0

    def check(ok: bool, msg: str) -> None:
        nonlocal fails
        print(("PASS: " if ok else "FAIL: ") + msg)
        fails += 0 if ok else 1

    def close(got: float, want: float) -> bool:
        return abs(got - want) <= max(REL * abs(want), ABS_S)

    S, B, D = a.steps, a.base_ms / 1e3, a.delta_ms / 1e3

    # ── compute time per rank and imbalance ──────────────────────────────
    want = [S * (B + r * D) for r in range(n)]
    for r in ranks:
        w = want[r["rank"]]
        check(close(r["compute_s"], w),
              f"rank {r['rank']}: compute {r['compute_s']:.4f} s vs injected {w:.4f} s")
    w_avg, w_max = sum(want) / n, max(want)
    w_imb = 100.0 * (w_max - w_avg) / w_max
    got_imb = par["compute"]["imbalance_pct"]
    check(abs(got_imb - w_imb) <= 2.0,
          f"compute imbalance {got_imb:.2f}% vs injected {w_imb:.2f}% (within 2 points)")
    slowest = par["compute"]["slowest_ranks"][0]["rank"]
    check(slowest == n - 1, f"slowest rank reported as {slowest} (want {n - 1})")

    # ── Allreduce wait = injected imbalance ──────────────────────────────
    funcs = {f["name"]: f for f in par["mpi"]["by_function"]}
    ar = funcs.get("MPI_Allreduce")
    check(ar is not None, "MPI_Allreduce present in by_function")
    if ar:
        check(ar["calls"] == n * S, f"MPI_Allreduce calls {ar['calls']} == {n * S} (no double counting)")
        w_max_wait = S * (n - 1) * D
        w_avg_wait = S * (n - 1) * D / 2
        check(close(ar["time_s"]["max"], w_max_wait),
              f"MPI_Allreduce max wait {ar['time_s']['max']:.4f} s vs injected {w_max_wait:.4f} s")
        check(close(ar["time_s"]["avg"], w_avg_wait),
              f"MPI_Allreduce avg wait {ar['time_s']['avg']:.4f} s vs injected {w_avg_wait:.4f} s")
    bar = funcs.get("MPI_Barrier")
    check(bar is not None and bar["calls"] == 3 * n,   # start-up + after each MPI-IO file
          f"MPI_Barrier calls {bar and bar['calls']} == {3 * n}")

    # ── cross-check against trace mode's Table 2 ─────────────────────────
    if a.trace_report and ar:
        txt = open(a.trace_report).read()
        m = re.search(r"^\s*(\d+)\s+([\d.]+)\s+\S+\s+\d+\s+\S+\s+MPI_Allreduce\s*$", txt, re.M)
        check(m is not None, "MPI_Allreduce row found in trace report")
        if m:
            t_calls, t_time = int(m.group(1)), float(m.group(2))
            s_time = ar["time_s"]["avg"] * n
            check(t_calls == ar["calls"], f"trace calls {t_calls} == snapshot calls {ar['calls']}")
            check(close(s_time, t_time),
                  f"snapshot Allreduce total {s_time:.4f} s vs trace {t_time:.4f} s (within 2%)")

    # ── I/O: exact counts ────────────────────────────────────────────────
    px, mi = par["io"]["posix"], par["io"]["mpiio"]
    exact = [
        (px["bytes"]["write"], n * POSIX_BYTES, "POSIX write bytes"),
        (px["bytes"]["read"],  n * POSIX_BYTES, "POSIX read bytes"),
        (px["calls"]["open"],  4 * n, "POSIX opens"),
        (px["calls"]["close"], 4 * n, "POSIX closes"),
        (px["calls"]["sync"],  n,     "POSIX syncs"),
        # write_at + write_all through a file view + iwrite_at (PnetCDF pattern)
        (mi["bytes"]["write"], 3 * n * MPIIO_BYTES, "MPI-IO write bytes"),
        (mi["bytes"]["read"],  n * MPIIO_BYTES, "MPI-IO read bytes"),
        (mi["calls"]["open"],  2 * n, "MPI-IO opens"),
        (mi["calls"]["close"], 2 * n, "MPI-IO closes"),
    ]
    for got, want_v, what in exact:
        check(got == want_v, f"{what}: {got} == {want_v}")

    print(f"\n{'ALL IMBALANCE CHECKS PASSED' if fails == 0 else f'{fails} CHECK(S) FAILED'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
