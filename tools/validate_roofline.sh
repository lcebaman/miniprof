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
getcap ./miniprof-nodemon | grep -q cap_perfmon || { echo "run: make nodemon-setcap"; exit 1; }
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

echo; echo "==================== report (memory-bound run) ===================="
python3 tools/miniprof_report.py miniprof_snapshot_roof_mem.json | sed -n '/^ Memory/,/^$/p;/^ Roofline/,/^$/p'
exit $FAIL
