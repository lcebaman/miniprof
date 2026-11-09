#!/usr/bin/env bash
# overhead.sh — wall-clock overhead of a miniprof mode vs MINIPROF_MODE=off
#
# Runs the same binary REPS times in each mode and compares the *fastest*
# run of each (min, not mean: run-to-run noise on a shared or
# oversubscribed machine is one-sided, it only ever adds time). Wall time
# is measured around mpirun, so launcher start-up is included in both
# sides — use a problem size that runs for at least a few seconds, or the
# start-up noise swamps the profiler's cost.
#
# Usage: overhead.sh NRANKS REPS MODE BINARY [mpirun flags...]
# Prints one line. Informational, not a gate: exits non-zero only if a
# run itself fails.

set -euo pipefail
NRANKS=$1; REPS=$2; MODE=$3; BIN=$4; shift 4

run_min() {
    local mode=$1 best=""
    shift   # remaining args are the mpirun flags
    for _ in $(seq "$REPS"); do
        local t0 t1 dt
        t0=$(date +%s%N)
        if ! MINIPROF_MODE=$mode MINIPROF_SNAPSHOT_OUTPUT=/dev/null \
             mpirun "$@" -n "$NRANKS" "$BIN" > /dev/null; then
            echo "overhead.sh: run failed (mode=$mode)" >&2
            exit 1
        fi
        t1=$(date +%s%N)
        dt=$(( (t1 - t0) / 1000000 ))
        if [ -z "$best" ] || [ "$dt" -lt "$best" ]; then best=$dt; fi
    done
    echo "$best"
}

base=$(run_min off "$@")
prof=$(run_min "$MODE" "$@")
awk -v b="$base" -v p="$prof" -v m="$MODE" -v r="$REPS" 'BEGIN {
    printf "overhead: mode=%s  off=%d ms  %s=%d ms  (best of %d)  overhead=%+.2f%%\n",
           m, b, m, p, r, (p - b) * 100.0 / b
}'
