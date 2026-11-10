#!/usr/bin/env python3
"""
validate_topdown.py — check a top-down snapshot of examples/topdown_validate
(MINIPROF_SNAPSHOT=topdown, >= 3 ranks) against the shape each region is
built to have (see the header of topdown_validate.c).

Checks (real counters):
  - every event set was measured; level 1 sums to ~100% of slots
  - alu      retiring is the largest level-1 category, >= 40%
  - fpchain  backend >= 40%, and more backend_cpu than backend_memory
  - chase    backend >= 60%, of which >= 70% backend_memory
  - stream   backend >= 40%, more backend_memory than backend_cpu
  - branch   bad speculation >= 10%, of which >= 70% mispredicts

--plumbing: software test events (MINIPROF_COUNTERS_TEST=1): only checks
that the sets rotated over threads and every block is present.

Exit 77 when top-down was not measured (no PMU, unsupported CPU).

Usage: validate_topdown.py SNAPSHOT.json [--plumbing]
"""

import argparse
import json
import sys

L1 = ("frontend_bound", "bad_speculation", "backend_bound", "retiring")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--plumbing", action="store_true")
    a = ap.parse_args()

    td = json.load(open(a.file))["metrics"].get("topdown")
    if not td or not td.get("available"):
        print(f"SKIP: top-down not measured: {(td or {}).get('reason', 'no topdown block')}")
        return 77

    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    sets = td["sets"]
    check(len(sets) == 3, f"3 event sets ({[s['name'] for s in sets]})")
    for s in sets:
        check(s["threads"] > 0, f"set {s['name']} measured by {s['threads']} thread(s) on {s['ranks']} rank(s)")
    regions = {r["name"]: r["topdown"] for r in td.get("regions", [])}
    for name in ("alu", "fpchain", "chase", "stream", "branch"):
        check(name in regions, f"region {name} present")

    if td.get("uarch") == "swtest":
        a.plumbing = True          # software test events: no shapes to check
    if a.plumbing:
        check(td["compute"]["level1"] is not None, "compute level1 present")
        print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
        return 0 if not fails else 1

    def row(name, t):
        l1, l2 = t.get("level1"), t.get("level2") or {}
        if not l1:
            print(f"  {name:<10} (no level 1)")
            return
        g = lambda k: l2.get(k)
        f = lambda v: f"{v:5.1f}" if isinstance(v, (int, float)) else "    -"
        print(f"  {name:<10} {f(l1['frontend_bound'])} {f(l1['bad_speculation'])} "
              f"{f(l1['backend_bound'])} {f(l1['retiring'])} | {f(g('frontend_latency'))} "
              f"{f(g('bad_speculation_mispredicts'))} {f(g('backend_memory'))} "
              f"{f(g('backend_cpu'))} {f(g('retiring_microcode'))}")

    print("\n  region      fe    bad   back  ret  |  fe_lat misp  b_mem b_cpu ucode   (% of slots)")
    row("compute", td["compute"])
    for n, t in regions.items():
        row(n, t)
    print()

    def l1(n):
        return (regions.get(n) or {}).get("level1") or {}

    def l2(n):
        return (regions.get(n) or {}).get("level2") or {}

    for n in ["compute"] + list(regions):
        t = td["compute"] if n == "compute" else regions[n]
        if t.get("level1"):
            tot = sum(t["level1"][k] for k in L1) + (t["level1"].get("smt_contention") or 0)
            check(90 <= tot <= 110, f"{n}: level 1 sums to {tot:.1f}% (90-110)")

    x = l1("alu")
    if x:
        top = max(L1, key=lambda k: x[k])
        check(top == "retiring" and x["retiring"] >= 40,
              f"alu: retiring {x['retiring']:.1f}% is the largest category (got {top})")
    x, y = l1("fpchain"), l2("fpchain")
    if x:
        check(x["backend_bound"] >= 40, f"fpchain: backend {x['backend_bound']:.1f}% >= 40")
        if "backend_cpu" in y:
            check(y["backend_cpu"] > y["backend_memory"],
                  f"fpchain: backend_cpu {y['backend_cpu']:.1f}% > backend_memory {y['backend_memory']:.1f}%")
    x, y = l1("chase"), l2("chase")
    if x:
        check(x["backend_bound"] >= 60, f"chase: backend {x['backend_bound']:.1f}% >= 60")
        if "backend_memory" in y:
            check(y["backend_memory"] >= 0.7 * x["backend_bound"],
                  f"chase: backend_memory {y['backend_memory']:.1f}% >= 70% of backend")
    x, y = l1("stream"), l2("stream")
    if x:
        check(x["backend_bound"] >= 40, f"stream: backend {x['backend_bound']:.1f}% >= 40")
        if "backend_memory" in y:
            check(y["backend_memory"] > y["backend_cpu"],
                  f"stream: backend_memory {y['backend_memory']:.1f}% > backend_cpu {y['backend_cpu']:.1f}%")
    x, y = l1("branch"), l2("branch")
    if x:
        check(x["bad_speculation"] >= 10, f"branch: bad speculation {x['bad_speculation']:.1f}% >= 10")
        if "bad_speculation_mispredicts" in y:
            check(y["bad_speculation_mispredicts"] >= 0.7 * x["bad_speculation"],
                  f"branch: mispredicts {y['bad_speculation_mispredicts']:.1f}% >= 70% of bad speculation")

    print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
