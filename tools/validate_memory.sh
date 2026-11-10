#!/bin/bash
# validate_memory.sh — P5 memory block on a real Zen 4 / Zen 5 node.
#
# Builds miniprof-nodemon, makes sure it holds CAP_PERFMON (sudo -n setcap
# if passwordless sudo is available, otherwise says what to run), then
# runs examples/membw_validate twice and checks the snapshot:
#   local   ranks bound to cores, first-touch memory: DRAM reads/writes vs
#           the known triad traffic, low remote-read share
#   remote  ranks on NUMA node 0, memory bound to the last NUMA node (the
#           other socket): remote-read share near 100%
#
# Usage: tools/validate_memory.sh [ranks] [N] [reps]   (from the repo root;
#        defaults 8 ranks, N = 32M doubles = 256 MiB per array, 40 reps)
set -u
R=${1:-8}; N=${2:-33554432}; K=${3:-40}
make nodemon examples/membw_validate >/dev/null || exit 1
H=./miniprof-nodemon
if ! getcap "$H" 2>/dev/null | grep -q cap_perfmon; then
    if sudo -n true 2>/dev/null; then
        chmod 0700 "$H" && sudo -n setcap cap_perfmon+ep "$H" || exit 1
    else
        echo "grant the helper its capability first:  sudo setcap cap_perfmon+ep $H"
        exit 1
    fi
fi
getcap "$H"
LAST=$(( $(ls -d /sys/devices/system/node/node* | wc -l) - 1 ))
OUT=miniprof_snapshot_mem
FAIL=0

echo; echo "==================== local: $R ranks, first-touch memory ===================="
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=${OUT}_local.json \
    mpirun -n "$R" --bind-to core ./examples/membw_validate "$N" "$K" || exit 1
python3 tools/validate_memory.py ${OUT}_local.json --ranks "$R" --n "$N" --reps "$K" || FAIL=1

echo; echo "==================== remote: cores on NUMA node 0, memory on node $LAST ===================="
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=${OUT}_remote.json \
    mpirun -n "$R" --bind-to none numactl --cpunodebind=0 --membind=$LAST \
    ./examples/membw_validate "$N" "$K" || exit 1
python3 tools/validate_memory.py ${OUT}_remote.json --ranks "$R" --n "$N" --reps "$K" --remote || FAIL=1

echo; echo "==================== report (local run) ===================="
python3 tools/miniprof_report.py ${OUT}_local.json | sed -n '/^ Memory/,/^$/p'
exit $FAIL
