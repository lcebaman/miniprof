#!/bin/bash
# validate_roofline.sh — P6 on a real node: calibrate, then a memory-bound
# and a compute-bound run on every physical core, each checked against the
# calibrated ceilings. Needs the FLOP counter (MINIPROF_SNAPSHOT=basic, the
# default) and miniprof-nodemon with CAP_PERFMON (make nodemon-setcap).
#
# Usage: tools/validate_roofline.sh [calibration dir]   (default calib/)
set -u
D=${1:-calib}
make calibrate nodemon examples/membw_validate examples/roofline_fma examples/counters_validate >/dev/null || exit 1
tools/nodemon_ready.sh ./miniprof-nodemon || exit 1
mkdir -p "$D"
H=$(hostname -s)
if [ ! -f "$D/calibration_$(hostname).json" ] && [ ! -f "$D/calibration_$H.json" ]; then
    echo "==================== calibrating (about a minute) ===================="
    ./miniprof-calibrate -o "$D" || exit 1
fi
P=$(lscpu | awk -F: '/^Core\(s\) per socket/{c=$2} /^Socket\(s\)/{s=$2} END{print c*s}')
FAIL=0
export MINIPROF_CALIBRATION=$D

echo; echo "==================== memory-bound: membw_validate, $P ranks ===================="
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=roofline MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_mem.json \
    mpirun -n "$P" --bind-to core ./examples/membw_validate 8388608 20 || exit 1
python3 tools/validate_roofline.py miniprof_snapshot_roof_mem.json --expect memory || FAIL=1

echo; echo "==================== compute-bound: roofline_fma, $P ranks ===================="
W=$(grep -q avx512f /proc/cpuinfo && echo fp64_512 || echo fp64_256)
MINIPROF_ROOFLINE_PEAK=$W MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=roofline MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_fma.json \
    mpirun -n "$P" --bind-to core ./examples/roofline_fma 3 || exit 1
MINIPROF_ROOFLINE_PEAK=$W python3 tools/validate_roofline.py miniprof_snapshot_roof_fma.json --expect compute || FAIL=1

echo; echo "==================== estimate: the memory-bound run without the helper ===================="
# MINIPROF_NODEMON pointing nowhere: DRAM bytes come from the L1D fill
# counter instead (the fallback). Its bytes must not exceed the measured
# ones (lower bound), so its intensity must not be below the measured one.
MINIPROF_NODEMON=/nonexistent MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_est.json \
    mpirun -n "$P" --bind-to core ./examples/membw_validate 8388608 20 >/dev/null || exit 1
python3 - miniprof_snapshot_roof_mem.json miniprof_snapshot_roof_est.json <<'PY' || FAIL=1
import json, sys
m = json.load(open(sys.argv[1]))["metrics"]["roofline"]
e = json.load(open(sys.argv[2]))["metrics"]["roofline"]
ok = True
def check(c, msg):
    global ok
    print(("  ok    " if c else "  FAIL  ") + msg); ok &= bool(c)
check(e.get("available") and e.get("estimate"), f"fallback used: {e.get('dram_bytes', e.get('reason'))}")
if m.get("estimate"):
    print("  skip  the reference run is itself an estimate (memory block partial or missing): no measurement to compare")
elif e.get("available") and m.get("available"):
    am, ae = m["nodes"][0]["arithmetic_intensity"], e["nodes"][0]["arithmetic_intensity"]
    bm, be = m["nodes"][0]["dram_gb_s"], e["nodes"][0]["dram_gb_s"]
    print(f"  measured {bm:.1f} GB/s, AI {am:.4f}; estimate {be:.1f} GB/s, AI {ae:.4f}: "
          f"fills see {100 * be / bm:.0f}% of the DRAM bytes")
    check(ae >= 0.95 * am, "estimated intensity is not below the measured one (upper bound)")
    check(be >= 0.3 * bm, "fills see at least 30% of the DRAM bytes (else the estimate is useless here)")
    check(e["nodes"][0]["bound"] == "memory", "estimate still says memory-bound")
sys.exit(0 if ok else 1)
PY

echo; echo "==================== estimate at MINIPROF_SNAPSHOT=roofline (fills + L2-prefetch misses) ===================="
MINIPROF_NODEMON=/nonexistent MINIPROF_SNAPSHOT=roofline MINIPROF_MODE=snapshot \
    MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_est2.json \
    mpirun -n "$P" --bind-to core ./examples/membw_validate 8388608 20 >/dev/null || exit 1
python3 - miniprof_snapshot_roof_mem.json miniprof_snapshot_roof_est2.json <<'PY' || FAIL=1
import json, sys
m = json.load(open(sys.argv[1]))["metrics"]
e = json.load(open(sys.argv[2]))["metrics"]["roofline"]
ok = True
def check(c, msg):
    global ok
    print(("  ok    " if c else "  FAIL  ") + msg); ok &= bool(c)
check(e.get("available") and e.get("estimate"), f"fallback used: {e.get('dram_bytes', e.get('reason'))}")
nd = m["memory"]["nodes"][0] if m["memory"].get("available") else None
if e.get("available") and nd:
    be = e["nodes"][0]["dram_gb_s"]
    ref, what = nd["read_gb_s"], "measured DRAM reads"
    if m["memory"].get("coverage"):
        # the helper sees NUMA node 0 only; first-touch spreads this run's
        # memory evenly over the NUMA nodes, so scale node 0 by their count
        k = json.load(open(sys.argv[2]))["nodes"][0].get("numa_nodes") or 1
        ref, what = ref * k, f"node-0 DRAM reads x {k} NUMA nodes (memory block covers node 0 only)"
    print(f"  {what}: {ref:.1f} GB/s; estimated reads {be:.1f} GB/s ({100 * be / ref:.0f}%)")
    check(0.85 <= be / ref <= 1.10, "core-counter reads within -15%..+10% of the DRAM reads")
sys.exit(0 if ok else 1)
PY

echo; echo "==================== per-region points: counters_validate, $P ranks ===================="
# MINIPROF_SNAPSHOT=roofline: regions get FLOPs per DRAM byte READ (fills +
# L2-prefetch misses). daxpy and dot both read 16 B per 2 FLOPs: 0.125.
# Arrays of 256 MiB each so both stream from DRAM.
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=roofline MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_reg.json \
    mpirun -n "$P" --bind-to core ./examples/counters_validate 33554432 10 >/dev/null || exit 1
python3 - miniprof_snapshot_roof_reg.json <<'PY' || FAIL=1
import json, sys
r = json.load(open(sys.argv[1]))["metrics"]["roofline"]
ok = True
def check(c, msg):
    global ok
    print(("  ok    " if c else "  FAIL  ") + msg); ok &= bool(c)
regs = {g["name"]: g for g in r.get("regions") or []}
print(f"  estimate {r.get('estimate')}; {r.get('regions_reason', '')}")
for n, g in regs.items():
    print(f"  {n:<12} {g['gflops_per_s_node']:9.1f} GFLOP/s/node  {g['dram_read_gb_s_node']:7.1f} GB/s read  "
          f"AI(read) {g['arithmetic_intensity_read']:.4f}  {g['bound']}  {g['pct_of_attainable']:.0f}% of attainable")
for n in ("daxpy", "dot_scalar"):
    g = regs.get(n)
    check(g is not None, f"region {n} on the roofline")
    if g:
        a = g["arithmetic_intensity_read"]
        check(0.125 * 0.85 <= a <= 0.125 * 1.15, f"{n}: FLOP per byte read {a:.4f} within 15% of 0.125")
        check(g["bound"] == "memory", f"{n}: memory-bound")
        check(g["pct_of_attainable"] <= 115, f"{n}: {g['pct_of_attainable']:.0f}% of attainable (<= 115)")
sys.exit(0 if ok else 1)
PY

echo; echo "==================== report (memory-bound run) ===================="
python3 tools/miniprof_report.py miniprof_snapshot_roof_mem.json | sed -n '/^ Memory/,/^$/p;/^ Roofline/,/^$/p'
exit $FAIL
