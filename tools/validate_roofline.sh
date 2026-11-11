#!/bin/bash
# validate_roofline.sh — P6 on a real node: calibrate, then a memory-bound
# and a compute-bound run on every physical core, each checked against the
# calibrated ceilings. Needs the FLOP counter (MINIPROF_SNAPSHOT=basic, the
# default) and miniprof-nodemon with CAP_PERFMON (make nodemon-setcap).
#
# Usage: tools/validate_roofline.sh [calibration dir]   (default calib/)
set -u
D=${1:-calib}
make calibrate nodemon examples/membw_validate examples/roofline_fma >/dev/null || exit 1
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
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_mem.json \
    mpirun -n "$P" --bind-to core ./examples/membw_validate 8388608 20 || exit 1
python3 tools/validate_roofline.py miniprof_snapshot_roof_mem.json --expect memory || FAIL=1

echo; echo "==================== compute-bound: roofline_fma, $P ranks ===================="
W=$(grep -q avx512f /proc/cpuinfo && echo fp64_512 || echo fp64_256)
MINIPROF_ROOFLINE_PEAK=$W MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_roof_fma.json \
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
if e.get("available") and m.get("available"):
    am, ae = m["nodes"][0]["arithmetic_intensity"], e["nodes"][0]["arithmetic_intensity"]
    bm, be = m["nodes"][0]["dram_gb_s"], e["nodes"][0]["dram_gb_s"]
    print(f"  measured {bm:.1f} GB/s, AI {am:.4f}; estimate {be:.1f} GB/s, AI {ae:.4f}: "
          f"fills see {100 * be / bm:.0f}% of the DRAM bytes")
    check(ae >= 0.95 * am, "estimated intensity is not below the measured one (upper bound)")
    check(be >= 0.3 * bm, "fills see at least 30% of the DRAM bytes (else the estimate is useless here)")
    check(e["nodes"][0]["bound"] == "memory", "estimate still says memory-bound")
sys.exit(0 if ok else 1)
PY

echo; echo "==================== report (memory-bound run) ===================="
python3 tools/miniprof_report.py miniprof_snapshot_roof_mem.json | sed -n '/^ Memory/,/^$/p;/^ Roofline/,/^$/p'
exit $FAIL
