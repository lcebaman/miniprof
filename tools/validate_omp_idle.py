#!/usr/bin/env python3
"""validate_omp_idle.py SNAPSHOT --threads T --rounds R --w-ms W --s-ms S

Known answer for examples/omp_idle_validate (run with MINIPROF_COUNTERS_TEST=1
and KMP_BLOCKTIME=infinite): per rank, compute = R (T W + S) and OpenMP
worker idle = R (T - 1) S CPU seconds. Exit 77 = SKIP (no OMPT or no
counters)."""
import argparse, json, sys

ap = argparse.ArgumentParser()
ap.add_argument("snapshot")
ap.add_argument("--threads", type=int, required=True)
ap.add_argument("--rounds", type=int, default=20)
ap.add_argument("--w-ms", type=float, default=20)
ap.add_argument("--s-ms", type=float, default=20)
ap.add_argument("--tol", type=float, default=0.10)
a = ap.parse_args()

doc = json.load(open(a.snapshot))
m = doc["metrics"]
omp, c = m.get("openmp") or {}, m.get("counters") or {}
if not omp.get("available"):
    print(f"SKIP: no OMPT ({omp.get('reason')})"); sys.exit(77)
if not c.get("available"):
    print(f"SKIP: counters unavailable ({c.get('reason')})"); sys.exit(77)

nr = doc["run"]["nranks"]
T, R, W, S = a.threads, a.rounds, a.w_ms / 1e3, a.s_ms / 1e3
want_comp = nr * R * (T * W + S)
want_idle = nr * R * (T - 1) * S
# swtest: "cycles" is task-clock in ns
comp = c["compute"]["cycles"] / 1e9
idle = c["omp_idle"]["cycles"] / 1e9
tot = c["totals"]["cycles"] / 1e9
fails = 0
def check(ok, msg):
    global fails
    print(("PASS: " if ok else "FAIL: ") + msg)
    fails += 0 if ok else 1
# compute also holds MPI_Init/Finalize-side work and loop overhead: allow
# +tol above and a little below
check(want_comp * (1 - a.tol / 2) <= comp <= want_comp * (1 + a.tol) + 0.05,
      f"compute {comp:.3f} CPU-s vs expected {want_comp:.3f} (R (T W + S) per rank)")
check(want_idle * (1 - a.tol) <= idle <= want_idle * (1 + a.tol) + 0.02,
      f"OpenMP worker idle {idle:.3f} CPU-s vs expected {want_idle:.3f} (R (T-1) S per rank)")
mpi = c["mpi"]["cycles"] / 1e9
check(abs(tot - (comp + idle + mpi)) < 1e-6 * max(tot, 1),
      f"totals {tot:.3f} = compute + mpi + omp_idle")
d = c["derived"]
check(abs(d["omp_idle_pct_of_cycles"] - 100 * idle / tot) < 0.1,
      f"omp_idle_pct_of_cycles {d['omp_idle_pct_of_cycles']:.1f}%")
print(f"\n{'ALL OMP IDLE CHECKS PASSED' if fails == 0 else f'{fails} CHECK(S) FAILED'}")
sys.exit(1 if fails else 0)
