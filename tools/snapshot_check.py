#!/usr/bin/env python3
"""
snapshot_check.py — structural validator for a miniprof snapshot JSON.

Used by `make run-3d-snapshot` / `make check`. Exits 0 if the document
is well-formed and self-consistent, 1 otherwise, printing one PASS/FAIL
line per check so CI logs show exactly what broke.

Usage:
    snapshot_check.py FILE [--nranks N] [--max-overhead FRACTION]
"""

import argparse
import json
import sys

SCHEMA = "miniprof-snapshot"
SUPPORTED_MAJOR = 0
METRIC_BLOCKS = ("parallel", "openmp", "counters", "topdown", "memory", "roofline")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--nranks", type=int, default=None,
                    help="expected number of MPI ranks")
    ap.add_argument("--max-overhead", type=float, default=0.05,
                    help="max allowed overhead.fraction_of_elapsed (default 0.05)")
    args = ap.parse_args()

    failures = 0

    def check(cond: bool, msg: str) -> None:
        nonlocal failures
        print(("PASS: " if cond else "FAIL: ") + msg)
        if not cond:
            failures += 1

    try:
        with open(args.file) as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"FAIL: cannot parse {args.file}: {e}")
        return 1
    check(True, f"{args.file} is valid JSON")

    check(doc.get("schema") == SCHEMA, f"schema == {SCHEMA!r}")
    ver = str(doc.get("schema_version", ""))
    check(ver.split(".")[0] == str(SUPPORTED_MAJOR),
          f"schema_version {ver!r} has supported major {SUPPORTED_MAJOR}")

    run = doc.get("run", {})
    ranks = doc.get("ranks", [])
    nodes = doc.get("nodes", [])
    nranks = run.get("nranks")

    check(isinstance(nranks, int) and nranks == len(ranks),
          f"run.nranks ({nranks}) == len(ranks) ({len(ranks)})")
    if args.nranks is not None:
        check(nranks == args.nranks, f"run.nranks ({nranks}) == expected ({args.nranks})")
    check(run.get("nnodes") == len(nodes),
          f"run.nnodes ({run.get('nnodes')}) == len(nodes) ({len(nodes)})")
    check(sum(n.get("ranks", 0) for n in nodes) == nranks,
          "sum(nodes[].ranks) == run.nranks")
    check([r.get("rank") for r in ranks] == list(range(len(ranks))),
          "ranks[] ordered 0..N-1")
    check(all(0 <= r.get("node", -1) < len(nodes) for r in ranks),
          "every rank's node index is valid")
    check([n.get("index") for n in nodes] == list(range(len(nodes))),
          "nodes[] ordered 0..M-1")

    el = run.get("elapsed_s", {})
    check(isinstance(el.get("min"), (int, float)) and 0 < el["min"] <= el.get("max", 0),
          f"0 < elapsed_s.min ({el.get('min')}) <= elapsed_s.max ({el.get('max')})")

    level = run.get("level")
    check(level in ("basic", "topdown", "roofline", "full"), f"run.level {level!r} is valid")

    for n in nodes:
        cpu = n.get("cpu", {})
        check(cpu.get("vendor") is not None,
              f"node {n.get('index')} ({n.get('hostname')}): cpu.vendor present "
              f"({cpu.get('vendor')}, uarch={cpu.get('uarch')})")

    frac = doc.get("overhead", {}).get("fraction_of_elapsed")
    check(isinstance(frac, (int, float)) and frac <= args.max_overhead,
          f"overhead.fraction_of_elapsed ({frac}) <= {args.max_overhead}")

    metrics = doc.get("metrics", {})
    check(isinstance(metrics.get("collected"), list), "metrics.collected is a list")
    missing = [k for k in METRIC_BLOCKS if k not in metrics]
    check(not missing, f"all metric blocks present in schema (missing: {missing or 'none'})")
    for k in metrics.get("collected", []):
        check(metrics.get(k) is not None, f"collected block {k!r} is non-null")
    omp = metrics.get("openmp")
    if isinstance(omp, dict):
        check(isinstance(omp.get("available"), bool), "openmp.available is a bool")
        if omp.get("available"):
            check(all(k in omp for k in ("threads", "parallel_s", "serial_s", "thread_time_s",
                                         "imbalance_pct", "regions")),
                  "openmp has threads/parallel_s/serial_s/thread_time_s/imbalance_pct/regions")
        else:
            check(bool(omp.get("reason")), "openmp.reason explains why it is unavailable")

    print(f"\n{'ALL SNAPSHOT CHECKS PASSED' if failures == 0 else f'{failures} SNAPSHOT CHECK(S) FAILED'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
