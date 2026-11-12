#!/usr/bin/env python3
"""
validate_roofline.py — check the roofline block (P6) of a known run.

  --expect memory   examples/membw_validate on every physical core:
                    bound == memory, >= 50% of the calibrated DRAM bandwidth
  --expect compute  examples/roofline_fma on every physical core, with
                    MINIPROF_ROOFLINE_PEAK set to the width it used:
                    bound == compute, >= 60% of the calibrated peak

--plumbing: test events (MINIPROF_COUNTERS_TEST/NODEMON_TEST) with a
quick calibration: only the structure is checked. Exit 77 = roofline
unavailable (no counters, no helper, no calibration) → SKIP.

Usage: validate_roofline.py SNAPSHOT.json (--expect memory|compute | --plumbing)
"""
import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--expect", choices=("memory", "compute"))
    ap.add_argument("--plumbing", action="store_true")
    a = ap.parse_args()
    r = json.load(open(a.file))["metrics"].get("roofline") or {}
    if not r.get("available"):
        print(f"SKIP: roofline not available: {r.get('reason', 'no roofline block')}")
        return 77
    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    c = r["ceilings"]
    check(c["peak_gflops"] > 0 and c["dram_triad_gb_s"] > 0,
          f"ceilings: peak {c['peak_gflops']:.1f} GFLOP/s ({c['peak_key']}), DRAM read {c['dram_read_gb_s']:.1f} / "
          f"triad {c['dram_triad_gb_s']:.1f} GB/s")
    check(len(r["nodes"]) >= 1, f"{len(r['nodes'])} node(s)")
    if r["calibration"].get("warning"):
        print(f"  note  {r['calibration']['warning']}")
    if not r["calibration"].get("same_cpu"):
        print(f"  note  calibration from a different CPU ({r['calibration']['host']})")
    if not a.plumbing:
        n = r["nodes"][0]
        est = bool(r.get("estimate"))
        if est:
            print(f"  note  {r.get('dram_bytes')}")
        mix = "read-only" if n.get("read_share_pct") is None else f"{n['read_share_pct']:.0f}% reads"
        print(f"  node {n['hostname']}: {n['gflops_per_s']:.1f} GFLOP/s, {n['dram_gb_s']:.1f} GB/s, "
              f"AI {n['arithmetic_intensity']:.3g} FLOP/B ({mix}, DRAM ceiling "
              f"{n['dram_ceiling_gb_s']:.0f} GB/s), {n['bound']}-bound, "
              f"{n['pct_of_attainable']:.1f}% of attainable, {n['pct_of_peak_gflops']:.1f}% of peak, "
              f"{n['pct_of_dram_bw']:.1f}% of DRAM bandwidth")
        check(n["bound"] == a.expect, f"bound: {n['bound']} (expected {a.expect})")
        if a.expect == "memory" and est:
            # an estimate counts reads only, while the stream also writes:
            # its share of the read-only roof is not a bandwidth measurement
            print(f"  info  {n['pct_of_dram_bw']:.1f}% of the read-only DRAM ceiling in reads alone "
                  "(estimate: writes not counted, so no >= 80% check)")
        elif a.expect == "memory":
            check(n["pct_of_dram_bw"] >= 80, f"{n['pct_of_dram_bw']:.1f}% of calibrated DRAM bandwidth (>= 80: a streaming run on every core)")
        else:
            check(n["pct_of_peak_gflops"] >= 60, f"{n['pct_of_peak_gflops']:.1f}% of calibrated peak (>= 60)")
        check(n["pct_of_attainable"] <= 115, f"{n['pct_of_attainable']:.1f}% of attainable (<= 115: ceilings hold)")
    print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
