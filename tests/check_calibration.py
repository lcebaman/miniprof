#!/usr/bin/env python3
"""check_calibration.py — structure and sanity of a miniprof-calibrate file."""
import json
import sys

d = json.load(open(sys.argv[1]))
fails = 0


def check(ok, msg):
    global fails
    print(("PASS: " if ok else "FAIL: ") + msg)
    fails += 0 if ok else 1


check(d.get("schema") == "miniprof-calibration", "schema")
n, c = d.get("node", {}), d.get("ceilings", {})
check(n.get("sockets", 0) >= 1 and n.get("cores_used", 0) >= 1, f"node: {n.get('sockets')} socket(s), {n.get('cores_used')} cores")
for k in ("dram_read_gb_s_node", "dram_triad_gb_s_node", "dram_read_gb_s_socket0"):
    check(c.get(k, 0) > 0, f"{k} = {c.get(k)}")
f = {k: v for k, v in c.items() if k.startswith("gflops_") and k.endswith("_node")}
check(len(f) >= 1, f"{len(f)} FLOP ceilings")
if "gflops_fp64_scalar_node" in c and "gflops_fp64_128_node" in c:
    check(c["gflops_fp64_128_node"] > c["gflops_fp64_scalar_node"], "128-bit FP64 beats scalar")
for k, v in f.items():
    one = c.get(k.replace("_node", "_one_core"), 0)
    check(v >= 0.9 * one, f"{k}: all cores {v:.1f} >= one core {one:.1f}")
print("ALL CALIBRATION CHECKS PASSED" if not fails else f"{fails} CALIBRATION CHECK(S) FAILED")
sys.exit(1 if fails else 0)
