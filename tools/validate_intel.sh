#!/usr/bin/env bash
# validate_intel.sh — validate miniprof on an Intel Granite Rapids node:
# runs the per-feature validation scripts one after the other and keeps
# everything in one log.
#
#   1. counters   tools/validate_counters.sh: exact region FLOPs (weighted
#                 FP_ARITH events) at basic/roofline/full, total vs perf stat
#   2. top-down   tools/validate_topdown.sh: encodings vs perf, levels 1/2 vs
#                 perf stat -M TopdownL1/L2, per-region shapes
#   3. memory     tools/validate_memory.sh: IMC CAS bytes vs a known triad
#                 (needs the helper's capability: passwordless sudo, or run
#                 `sudo setcap cap_perfmon+ep ./miniprof-nodemon` first)
#   4. roofline   tools/validate_roofline.sh: calibrate, memory- and
#                 compute-bound runs against the ceilings
#
# Run from the repo root on an exclusive compute node, inside an allocation:
#     ./tools/validate_intel.sh            # all four
#     ./tools/validate_intel.sh counters topdown
# Writes validate_intel_<host>.txt; send it back.
set -u
LOG="validate_intel_$(hostname -s).txt"
STEPS=${*:-counters topdown memory roofline}
{
echo "== $(date -u) $(hostname) $(git log --oneline -1 2>/dev/null)"
grep -m1 'model name' /proc/cpuinfo
make -s lib nodemon calibrate examples >/dev/null || { echo "build failed"; exit 1; }
for s in $STEPS; do
  echo; echo "################################ $s ################################"
  case $s in
    counters) SKIP_CHECK=1 ./tools/validate_counters.sh ;;
    topdown)  ./tools/validate_topdown.sh ;;
    memory)   ./tools/validate_memory.sh ;;
    roofline) ./tools/validate_roofline.sh ;;
    *) echo "unknown step $s (counters topdown memory roofline)" ;;
  esac
  echo "(exit $?)"
done
} 2>&1 | tee "$LOG"
echo "wrote $LOG"
