#!/usr/bin/env python3
"""check_fingerprint.py — miniprof_fingerprint.py on fixtures and on every
snapshot given: every axis is null or within 0-100, the class names the
largest component, the components add back to ~100% of elapsed, the
compute class follows the documented rules on the full demo fixture, and
the CLI writes well-formed JSON, CSV and HTML for several runs at once."""
import csv, json, os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, "..", "tools"))
from check_report_html import Check  # noqa: E402
import miniprof_fingerprint as FP  # noqa: E402

tool = os.path.join(here, "..", "tools", "miniprof_fingerprint.py")
fails = 0


def check(ok, msg):
    global fails
    print(("ok    " if ok else "FAIL  ") + msg)
    fails += 0 if ok else 1


demo = os.path.join(here, "fixtures", "report_demo.json")
paths = [demo] + sys.argv[1:]
for p in paths:
    d = json.load(open(p))
    f = FP.fingerprint(d)
    name = os.path.basename(p)
    bad = [k for k, x in f["axes"].items() if x is not None and not 0 <= x <= 100]
    check(not bad, f"{name}: axes within 0-100 {bad or ''}")
    if not (d.get("metrics") or {}).get("parallel"):
        continue
    comps = f["components"]
    check(comps and comps[0]["pct"] == max(c["pct"] for c in comps) and f["class"].startswith(comps[0]["name"]),
          f"{name}: class leads with the largest component ({f['class']})")
    tot = sum(c["pct"] for c in comps)
    # serial OpenMP and barrier wait come out of compute; the rest are disjoint shares of elapsed
    check(80 <= tot <= 101, f"{name}: components cover the run ({tot:.1f}% of elapsed)")
    m = d.get("metrics") or {}
    if not (m.get("topdown") or {}).get("available"):
        check(all(f["axes"][k] is None for k in ("retiring", "frontend", "backend_memory")),
              f"{name}: no top-down block, core axes null")
    if not (m.get("roofline") or {}).get("available") and not (m.get("memory") or {}).get("available"):
        check(f["axes"]["dram_bw"] is None and f["axes"]["flops_peak"] is None,
              f"{name}: no roofline or memory block, DRAM and FLOP axes null")

# the demo fixture: top-down + memory + roofline, memory-side, 31% of DRAM bandwidth,
# backend memory the largest stall -> memory-latency bound
f = FP.fingerprint(json.load(open(demo)))
c = next(x for x in f["components"] if x["name"] == "compute")
check(c["detail"] == "memory-latency bound", f"demo: compute class is memory-latency bound ({c['detail']})")
check(abs(f["axes"]["dram_bw"] - 31.0) < 0.1 and abs(f["axes"]["backend_memory"] - 38.1) < 0.01,
      "demo: DRAM and backend-memory axes taken from roofline and top-down")
m = next(x for x in f["components"] if x["name"] == "MPI")
check(abs(m["pct"] - (f["axes"]["mpi"] - f["axes"]["imbalance"])) < 0.01,
      "demo: imbalance wait is carved out of MPI time")
check(m["detail"] == "bandwidth (large messages)", f"demo: MPI detail from the p2p size histogram ({m['detail']})")

# rule edges, on synthetic axes
ax = {k: None for k, *_ in FP.AXES}
check(FP.compute_class(dict(ax, dram_bw=75.0), {}, {}) == "memory-bandwidth bound", "rule: >= 60% DRAM -> bandwidth bound")
check(FP.compute_class(dict(ax, flops_peak=70.0, dram_bw=10.0), {}, {}) == "FLOP bound", "rule: >= 50% peak -> FLOP bound")
td = dict(ax, retiring=20.0, frontend=10.0, bad_spec=2.0, backend_memory=15.0, backend_core=50.0)
check(FP.compute_class(td, {}, {"x": 1}).startswith("core bound"), "rule: backend core largest -> core bound")
check(FP.compute_class(ax, {}, {}).startswith("not characterised"), "rule: nothing measured -> not characterised")

# CLI: several runs, JSON/CSV/HTML outputs
with tempfile.TemporaryDirectory() as td:
    j, c, h = (os.path.join(td, n) for n in ("f.json", "f.csv", "f.html"))
    runs = [demo, os.path.join(here, "fixtures", "report_demo_b.json")] + sys.argv[1:3]
    r = subprocess.run([sys.executable, tool] + runs + ["--json", j, "--csv", c, "--html", h],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    check(r.returncode == 0 and "Class" in r.stdout, "CLI: runs on several snapshots" + (": " + r.stderr if r.returncode else ""))
    if r.returncode == 0:
        doc = json.load(open(j))
        check(doc.get("schema") == "miniprof-fingerprint" and len(doc["runs"]) == len(runs), "CLI: JSON has one entry per run")
        rows = list(csv.reader(open(c)))
        check(len(rows[0]) == 3 + len(runs) and any(rw[0] == "class" for rw in rows), "CLI: CSV has a column per run")
        chk = Check()
        chk.feed(open(h).read())
        check(not chk.err and not chk.stack, "CLI: HTML well formed" + (f" {chk.err[:3]}" if chk.err else ""))
    r = subprocess.run([sys.executable, tool, demo, demo, "--names", "only-one"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    check(r.returncode == 1, "CLI: --names count mismatch is an error")

print(f"{'FAIL' if fails else 'PASS'}: fingerprint ({fails} failure(s))")
sys.exit(1 if fails else 0)
