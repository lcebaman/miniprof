#!/usr/bin/env python3
"""check_diff.py — miniprof_diff.py on fixture pairs: runs, text has the
expected sections, the compute-time factors multiply back to the time
ratio, a snapshot against itself shows ratio 1 and no change, and the
HTML page is well formed (same checks as check_report_html.py)."""
import json, os, re, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from check_report_html import Check  # noqa: E402

tool = os.path.join(here, "..", "tools", "miniprof_diff.py")
fails = 0


def check(ok, msg):
    global fails
    print(("ok    " if ok else "FAIL  ") + msg)
    fails += 0 if ok else 1


def run(a, b):
    with tempfile.TemporaryDirectory() as td:
        h = os.path.join(td, "d.html")
        r = subprocess.run([sys.executable, tool, a, b, "--html", h], capture_output=True, text=True)
        page = open(h).read() if os.path.exists(h) else ""
    return r, page


fa = os.path.join(here, "fixtures", "report_demo.json")
fb = os.path.join(here, "fixtures", "report_demo_b.json")
r, page = run(fa, fb)
check(r.returncode == 0, f"diff A B exits 0 {r.stderr[-300:]}")
for sec in ("Where the time went", "Compute time explained", "Top-down, compute", "Regions",
            "MPI functions", "Memory", "Roofline"):
    check(sec in r.stdout, f"text has section '{sec}'")
m = re.search(r"compute time ratio\s+([\d.]+)\s+\(product of the factors: ([\d.]+)\)", r.stdout)
check(m and abs(float(m.group(1)) - float(m.group(2))) < 0.002, "factors multiply back to the compute time ratio")
check("1.29x slower" in r.stdout, "verdict: B 1.29x slower")
c = Check(); c.feed(page)
check(not c.err and not c.stack, f"HTML well formed {c.err[:2]} {c.stack[-3:]}")
check(page.count("<script") == 1 and "http://" not in page and "https://" not in page, "HTML self-contained")
for sid in ("time", "factors", "topdown", "regions", "mpi", "memory", "roofline"):
    check(sid in c.ids, f"HTML section #{sid}")

r, page = run(fa, fa)
check(r.returncode == 0, "diff A A exits 0")
check("same time within 2%" in r.stdout, "A vs itself: same time")
m = re.search(r"compute time ratio\s+([\d.]+)", r.stdout)
check(m and abs(float(m.group(1)) - 1) < 1e-6, "A vs itself: compute ratio 1")

for extra in sys.argv[1:]:          # real snapshots from the suite: must not crash, against the demo or themselves
    r, page = run(extra, extra)
    c = Check(); c.feed(page)
    check(r.returncode == 0 and not c.err and not c.stack, f"diff {os.path.basename(extra)} with itself")
print("ALL DIFF CHECKS PASSED" if not fails else f"{fails} DIFF CHECK(S) FAILED")
sys.exit(1 if fails else 0)
