#!/usr/bin/env python3
"""
validate_memory.py — check the snapshot memory block (P5) of a run of
examples/membw_validate against the traffic it is known to generate
(see the header of membw_validate.c).

Checks (real counters, one node):
  - DRAM reads  within -5% .. +10% of ranks x REPS x 24 N bytes
  - DRAM writes within -5% .. +25% of ranks x REPS x  8 N bytes
    (initialisation and page zeroing add writes once)
  - peak bandwidth >= average; package power plausible (20-2000 W/node)
  - --remote: memory bound to another socket → the cores' socket reads
    >= 80% remote; otherwise (local first touch) <= 20% remote

--plumbing: software test events (MINIPROF_NODEMON_TEST=1): only the
structure of the block is checked.

Exit 77 when the memory block is unavailable (no helper, no capability,
unsupported CPU): the make target reports SKIP with the reason.

Usage: validate_memory.py SNAPSHOT.json --ranks R --n N --reps K [--remote] [--plumbing]
"""

import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--ranks", type=int, default=0)
    ap.add_argument("--n", type=int, default=0)
    ap.add_argument("--reps", type=int, default=0)
    ap.add_argument("--remote", action="store_true")
    ap.add_argument("--plumbing", action="store_true")
    a = ap.parse_args()

    m = json.load(open(a.file))["metrics"].get("memory") or {}
    if not m.get("available"):
        print(f"SKIP: memory counters not measured: {m.get('reason', 'no memory block')}")
        return 77

    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    nodes = m["nodes"]
    check(len(nodes) >= 1, f"{len(nodes)} node(s) measured")
    n0 = nodes[0]
    check(len(n0["sockets"]) >= 1, f"{len(n0['sockets'])} socket(s)")
    check(n0["samples"] > 0, f"{n0['samples']} sampling intervals")
    for s in n0["sockets"]:
        check(s["channels"]["count"] >= 1, f"socket {s['socket']}: {s['channels']['count']} channels")

    if a.plumbing:
        print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
        return 0 if not fails else 1

    el = n0["elapsed_s"]
    rd = n0["read_gb_s"] * el
    wr = n0["write_gb_s"] * el
    erd = a.ranks * a.reps * 24.0 * a.n * 1e-9
    ewr = a.ranks * a.reps * 8.0 * a.n * 1e-9
    print(f"\n  node {n0['hostname']}: {el:.2f} s, read {rd:.2f} GB (loop {erd:.2f}), "
          f"write {wr:.2f} GB (loop {ewr:.2f}), avg {n0['total_gb_s']:.1f} GB/s, "
          f"peak {n0['peak_gb_s']:.1f} GB/s, {n0['power_w']:.0f} W")
    for s in n0["sockets"]:
        ch = s["channels"]
        print(f"  socket {s['socket']}: read {s['read_gb_s']:.2f} GB/s (peak {s['peak_read_gb_s']:.2f}), "
              f"write {s['write_gb_s']:.2f}, remote "
              + ("-" if s["remote_read_share_pct"] is None else f"{s['remote_read_share_pct']:.1f}%") + ", "
              f"{ch['active']}/{ch['count']} channels active, {s['power_w']:.0f} W")
    print()
    check(0.95 * erd <= rd <= 1.10 * erd, f"DRAM reads {rd:.2f} GB within -5%..+10% of {erd:.2f}")
    check(0.95 * ewr <= wr <= 1.25 * ewr, f"DRAM writes {wr:.2f} GB within -5%..+25% of {ewr:.2f}")
    check(n0["peak_gb_s"] >= n0["total_gb_s"], f"peak {n0['peak_gb_s']:.1f} >= average {n0['total_gb_s']:.1f} GB/s")
    check(20 <= n0["power_w"] <= 2000, f"package power {n0['power_w']:.0f} W plausible")

    # the socket whose cores did the work: the one with most remote reads
    # (remote run) or most DRAM reads (local run)
    socks = n0["sockets"]
    if a.remote:
        s = max(socks, key=lambda x: x["remote_read_gb_s"])
        if s["remote_read_share_pct"] is None:
            s["remote_read_share_pct"] = 0.0
        check(s["remote_read_share_pct"] >= 80,
              f"socket {s['socket']}: {s['remote_read_share_pct']:.1f}% of its reads remote (>= 80, memory bound to the other socket)")
    else:
        busy = [x for x in socks if x["read_gb_s"] > 0.2 * max(y["read_gb_s"] for y in socks)
                and x["remote_read_share_pct"] is not None]
        for s in busy:
            check(s["remote_read_share_pct"] <= 20,
                  f"socket {s['socket']}: {s['remote_read_share_pct']:.1f}% of its reads remote (<= 20, first touch is local)")

    print("PASS" if not fails else f"FAIL ({len(fails)} checks)")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
