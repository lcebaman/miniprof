#!/usr/bin/env bash
# validate_counters.sh — P3 counter validation on a real node (Zen 4 / Zen 5)
#
# Run from the repo root on a COMPUTE node with MPI and perf available,
# inside an allocation (e.g. salloc -N1 --exclusive, then this script):
#
#     ./tools/validate_counters.sh
#
# Settings (environment):
#   LAUNCH   launcher prefix taking a rank count   (default "mpirun -n";
#            with Slurm: LAUNCH="srun -n")
#   NP       ranks for the multi-rank tests        (default 4)
#   CV_N, CV_REPS   counters_validate size         (default 2097152 20)
#   SKIP_CHECK=1    skip the full `make check` at the end
#
# Writes counters_validate_<host>.txt (send it back) plus the snapshot
# JSON files it checked.

set -u
HOST=$(hostname -s)
LOG="counters_validate_${HOST}.txt"
LAUNCH=${LAUNCH:-"mpirun -n"}
NP=${NP:-4}
CV_N=${CV_N:-2097152}
CV_REPS=${CV_REPS:-20}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILS=0

sec()  { printf '\n==================== %s ====================\n' "$1"; }
step() { printf '\n$ %s\n' "$*"; "$@"; local rc=$?; [ $rc -ne 0 ] && echo "(exit $rc)"; return $rc; }
mark() { [ "$1" -eq 0 ] || FAILS=$((FAILS + 1)); }

{
sec "environment"
date -u; hostname; uname -r
grep -m1 'model name' /proc/cpuinfo
for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/nmi_watchdog \
         /sys/bus/event_source/devices/cpu/rdpmc; do
  printf '%-48s %s\n' "$f" "$(cat "$f" 2>&1)"
done
perf --version 2>&1; git log --oneline -1 2>/dev/null
echo "LAUNCH='$LAUNCH' NP=$NP CV_N=$CV_N CV_REPS=$CV_REPS"

sec "build"
step make -s lib || { echo "build failed"; exit 1; }
step make -s -C examples counters_validate mpi_percall imbalance_validate || { echo "build failed"; exit 1; }

sec "perf stat reference: user-space FLOPs, whole run, MINIPROF_MODE=off"
cat > "$TMP/wrap.sh" <<WRAP
#!/bin/sh
exec perf stat -x, -e fp_ret_sse_avx_ops.all:u -o "$TMP/perf.\$\$.txt" "\$@"
WRAP
chmod +x "$TMP/wrap.sh"
MINIPROF_MODE=off $LAUNCH "$NP" "$TMP/wrap.sh" ./examples/counters_validate "$CV_N" "$CV_REPS"
PERF_FLOPS=$(cat "$TMP"/perf.*.txt 2>/dev/null | grep -v '^#' | grep fp_ret | \
             awk -F, '$1 ~ /^[0-9]+$/ {s += $1} END {printf "%.0f", s}')
echo "perf stat FLOPs summed over $(ls "$TMP"/perf.*.txt 2>/dev/null | wc -l) ranks: ${PERF_FLOPS:-none}"
cat "$TMP"/perf.*.txt 2>/dev/null | grep -v '^#' | head -4

for lvl in basic roofline; do
  sec "snapshot MINIPROF_SNAPSHOT=$lvl"
  J="miniprof_snapshot_cv_${lvl}_${HOST}.json"
  MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=$lvl MINIPROF_SNAPSHOT_OUTPUT="$J" MINIPROF_VERBOSE=1 \
    $LAUNCH "$NP" ./examples/counters_validate "$CV_N" "$CV_REPS" 2>&1 | grep -v '^\[miniprof\] rank [1-9]'
  EXTRA=""
  [ "$lvl" = basic ] && [ -n "${PERF_FLOPS:-}" ] && [ "${PERF_FLOPS:-0}" != 0 ] && EXTRA="--perf-flops $PERF_FLOPS"
  python3 tools/validate_counters.py "$J" --n "$CV_N" --reps "$CV_REPS" $EXTRA; rc=$?
  [ $rc -eq 77 ] && rc=1   # on a real Zen node "unavailable" is a failure
  mark $rc
  python3 -c "import json,sys; c=json.load(open('$J'))['metrics']['counters']; \
print(json.dumps({k: c.get(k) for k in ('uarch','group','read_method','reads','events','derived')}, indent=1))"
done

sec "Zen 5 FLOP precision filter (for P6): fp_ret_sse_avx_ops umask variants on daxpy"
# Kernel JSON lists packed_double_flops as umask 0xa0, which counted 0 on
# EPYC 9555; the precision field may need the op-type bits as well.
cat > "$TMP/daxpy.c" <<'DAX'
#include <stdio.h>
#include <stdlib.h>
#define N (4 * 1024 * 1024)
int main(void) {
    double *x = malloc(N * sizeof(double)), *y = malloc(N * sizeof(double)), a = 1.0000001;
    float *xf = malloc(N * sizeof(float)), *yf = malloc(N * sizeof(float)), af = 1.0001f;
    for (long i = 0; i < N; i++) { x[i] = 1e-9; y[i] = 1.0; xf[i] = 1e-6f; yf[i] = 1.0f; }
    for (int r = 0; r < 20; r++) for (long i = 0; i < N; i++) y[i] = a * x[i] + y[i];
    for (int r = 0; r < 10; r++) for (long i = 0; i < N; i++) yf[i] = af * xf[i] + yf[i];
    printf("expected: FP64 %ld, FP32 %ld flops (+ init)\n", 2L * N * 20, 2L * N * 10);
    return (int)(y[7] + yf[7]) == 12345;
}
DAX
cc -O3 -march=native -o "$TMP/daxpy_mix" "$TMP/daxpy.c" 2>&1
"$TMP/daxpy_mix"
for um in 0x0f 0x1f 0x40 0x4f 0x60 0x6f 0x80 0x8f 0xa0 0xaf; do
  r=$(perf stat -x, -e "cpu/event=0x03,umask=$um/u" -- "$TMP/daxpy_mix" 2>&1 >/dev/null | grep -v '^#' | tail -1)
  printf 'umask=%-5s %s\n' "$um" "$r"
done

sec "per-call wrapper cost (MPI_Iprobe, 1 rank)"
for cfg in "off" "trace" "snapshot" "snapshot NO_RDPMC"; do
  set -- $cfg
  printf '%-22s ' "$cfg"
  env MINIPROF_MODE=$1 ${2:+MINIPROF_NO_RDPMC=1} MINIPROF_TRACE_FUNCS=/dev/null \
      MINIPROF_TRACE_OUTPUT=/dev/null MINIPROF_SNAPSHOT_OUTPUT=/dev/null \
      $LAUNCH 1 ./examples/mpi_percall 2>/dev/null | grep ns/call
done

sec "NUMA: remote DRAM fill share (1 rank, 256 MiB arrays)"
if command -v numactl >/dev/null && [ "$(ls -d /sys/devices/system/node/node[0-9]* | wc -l)" -ge 2 ]; then
  for mem in 0 1; do
    J="$TMP/numa_$mem.json"
    MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT="$J" \
      $LAUNCH 1 numactl --cpunodebind=0 --membind=$mem ./examples/counters_validate 33554432 4 >/dev/null 2>&1
    python3 -c "import json; d=json.load(open('$J'))['metrics']['counters']['derived']; \
r=d.get('remote_dram_fill_pct'); \
print('cpu node 0, memory node $mem: ' + ('remote_dram_fill_pct=%.1f' % r if r is not None else \
'remote fills not counted (optional event dropped to fit the PMU, e.g. NMI watchdog)') + \
'  dram_fill_gb_per_s=%.2f' % d.get('dram_fill_gb_per_s', -1))" 2>&1
  done
else
  echo "skipped (numactl missing or single NUMA node)"
fi

sec "snapshot report on imbalance_validate (text summary incl. counters)"
J="miniprof_snapshot_iv_${HOST}.json"
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT="$J" $LAUNCH 2 ./examples/imbalance_validate 20 10 10 >/dev/null 2>&1
python3 tools/miniprof_report.py "$J"

if [ "${SKIP_CHECK:-0}" != 1 ]; then
  sec "make check (NRANKS=$NP)"
  make check NRANKS="$NP" 2>&1 | grep -E '^(PASS|FAIL|SKIP)|CHECKS|rror' | sort | uniq -c
fi

sec "summary"
[ $FAILS -eq 0 ] && echo "COUNTER VALIDATION PASSED" || echo "COUNTER VALIDATION: $FAILS FAILED STEP(S)"
} 2>&1 | tee "$LOG"
echo "Wrote $LOG — please send it back."
