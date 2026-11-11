#!/usr/bin/env bash
# pmu_probe.sh — collect what miniprof's counter layer needs to know about
# a node: CPU identity, perf access, PMU sysfs formats, which named and raw
# events open and count, rdpmc availability, and a known-FLOP check.
#
# Run it on a COMPUTE node (not a login node), ideally exclusive:
#     srun -N1 -n1 --exclusive tools/pmu_probe.sh      # or just ./tools/pmu_probe.sh
# Writes pmu_probe_<host>.txt in the current directory. No root needed;
# anything that fails is recorded as such and the script carries on.

OUT="pmu_probe_$(hostname -s).txt"
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sec() { printf '\n==================== %s ====================\n' "$1"; }
run() { printf '\n$ %s\n' "$*"; "$@" 2>&1; }

{
sec "system"
run date -u
run hostname
run uname -a
grep -h PRETTY_NAME /etc/os-release 2>/dev/null
run lscpu
echo; awk '/^processor/{n++} n==1' /proc/cpuinfo | grep -E '^(vendor_id|cpu family|model|model name|stepping|microcode)'
echo "flags of interest:"; grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | \
  grep -xE 'perfctr_core|perfctr_nb|perfctr_llc|perfmon_v2|rdpru|avx2|fma|avx512f|avx512_bf16|amd_lbr_v2|ibs|cpuid_fault' | tr '\n' ' '; echo

sec "perf access"
for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/perf_event_max_sample_rate \
         /proc/sys/kernel/kptr_restrict /proc/sys/kernel/nmi_watchdog \
         /sys/bus/event_source/devices/cpu/rdpmc; do
  printf '%-55s %s\n' "$f" "$(cat "$f" 2>&1)"
done
run id
command -v getpcaps >/dev/null && run getpcaps $$

sec "PMUs (sysfs)"
for d in /sys/bus/event_source/devices/*; do
  n=$(basename "$d")
  printf '\n--- %s  type=%s' "$n" "$(cat "$d/type" 2>/dev/null)"
  [ -f "$d/cpumask" ] && printf '  cpumask=%s' "$(head -c 60 "$d/cpumask")"
  echo
  for f in "$d"/format/* "$d"/caps/*; do
    [ -f "$f" ] && printf '   %-12s %-24s %s\n' "$(basename "$(dirname "$f")")" "$(basename "$f")" "$(cat "$f" 2>/dev/null)"
  done
  case "$n" in cpu|software|tracepoint|breakpoint|kprobe|uprobe) ;;
    *) [ -d "$d/events" ] && { printf '   events: '; ls "$d/events" | grep -v '\.\(unit\|scale\)$' | tr '\n' ' '; echo; } ;;
  esac
done

sec "perf tool"
command -v perf || echo "perf NOT FOUND in PATH"
run perf --version

# ── known-FLOP kernel ────────────────────────────────────────────────────
cat > "$TMP/daxpy.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (4 * 1024 * 1024)
#define REPS 50
int main(void) {
    double *x = malloc(N * sizeof(double)), *y = malloc(N * sizeof(double)), a = 1.0000001;
    for (long i = 0; i < N; i++) { x[i] = i * 1e-9; y[i] = 1.0; }
    for (int r = 0; r < REPS; r++)
        for (long i = 0; i < N; i++) y[i] = a * x[i] + y[i];
    double s = 0; for (long i = 0; i < N; i++) s += y[i];
    printf("daxpy: expected FP64 flops in main loop = %ld (checksum %.6g)\n", 2L * N * REPS, s);
    return 0;
}
EOF
$CC -O3 -march=native -o "$TMP/daxpy_vec" "$TMP/daxpy.c" 2>&1
$CC -O1 -fno-tree-vectorize -mno-fma -o "$TMP/daxpy_scalar" "$TMP/daxpy.c" 2>&1
ls -la "$TMP"/daxpy_* 2>&1 | awk '{print $NF}'

sec "named events: open and count (each on its own)"
EVENTS="cycles instructions ls_not_halted_cyc ex_ret_instr ex_ret_ops
fp_ret_sse_avx_ops.all fp_ret_sse_avx_ops.mac_flops fp_ret_sse_avx_ops.add_sub_flops fp_ret_sse_avx_ops.mult_flops
fp_ret_sse_avx_ops.scalar_double_flops fp_ret_sse_avx_ops.packed_double_flops
fp_ret_sse_avx_ops.scalar_single_flops fp_ret_sse_avx_ops.packed_single_flops
fp_ops_retired_by_width.scalar_uops_retired fp_ops_retired_by_width.pack_128_uops_retired
fp_ops_retired_by_width.pack_256_uops_retired fp_ops_retired_by_width.pack_512_uops_retired
l2_cache_req_stat.ic_dc_miss_in_l2 l2_cache_req_stat.dc_access_in_l2
ls_any_fills_from_sys.dram_io_all ls_any_fills_from_sys.dram_io_far ls_any_fills_from_sys.all
ls_dmnd_fills_from_sys.dram_io_all ls_l1_d_tlb_miss.all_l2_miss
de_no_dispatch_per_slot.no_ops_from_frontend de_no_dispatch_per_slot.backend_stalls
de_no_dispatch_per_slot.smt_contention de_src_op_disp.all ex_no_retire.load_not_complete
msr/aperf/ msr/mperf/ msr/tsc/ msr/irperf/"
for e in $EVENTS; do
  r=$(perf stat -x, -e "$e" -- "$TMP/daxpy_vec" 2>&1 >/dev/null | grep -v '^#' | tail -1)
  printf '%-48s %s\n' "$e" "$r"
done

sec "raw encodings (cpu/event=,umask=/) on the vectorised daxpy"
# Zen4 FLOPs-all umask is 0x1f, Zen5 is 0x0f; both are tried.
for raw in "cpu/event=0x76/" "cpu/event=0xc0/" "cpu/event=0x03,umask=0x1f/" "cpu/event=0x03,umask=0x0f/" \
           "cpu/event=0x08,umask=0x04/" "cpu/event=0x08,umask=0x10/" "cpu/event=0x08,umask=0x20/" \
           "cpu/event=0x64,umask=0x09/" "cpu/event=0x44,umask=0x48/" "cpu/event=0x44,umask=0x40/" \
           "cpu/event=0x1a0,umask=0x1e/"; do
  r=$(perf stat -x, -e "$raw" -- "$TMP/daxpy_vec" 2>&1 >/dev/null | grep -v '^#' | tail -1)
  printf '%-36s %s\n' "$raw" "$r"
done

sec "FLOP check: one 6-event group, vectorised then scalar daxpy"
G="{cycles,instructions,fp_ret_sse_avx_ops.all,fp_ops_retired_by_width.scalar_uops_retired,fp_ops_retired_by_width.pack_256_uops_retired,fp_ops_retired_by_width.pack_512_uops_retired}"
run perf stat -e "$G" -- "$TMP/daxpy_vec"
run perf stat -e "$G" -- "$TMP/daxpy_scalar"
echo; echo "disassembly hint (vectorised inner loop instructions):"
objdump -d --no-show-raw-insn "$TMP/daxpy_vec" 2>/dev/null | grep -oE 'vfmadd[0-9a-z]+ +[^,]*' | sort | uniq -c | head -5

sec "7th event in one group (expect failure / multiplexing on a 6-counter PMU)"
run perf stat -e "{cycles,instructions,ex_ret_ops,fp_ret_sse_avx_ops.all,ls_any_fills_from_sys.all,l2_cache_req_stat.all,de_src_op_disp.all}" -- "$TMP/daxpy_vec"

sec "frequency (msr PMU) and pipeline metrics (for P4)"
run perf stat -e msr/aperf/,msr/mperf/,msr/tsc/,cycles -- "$TMP/daxpy_vec"
run perf stat -M PipelineL1 -- "$TMP/daxpy_vec"
run perf stat -M PipelineL2 -- "$TMP/daxpy_vec"

sec "perf's own definitions for this CPU (new generations: encodings + formulas)"
# every core event miniprof's tables use, as perf encodes it on this CPU
for ev in ls_not_halted_cyc ex_ret_instr ex_ret_ops de_src_op_disp.all \
          de_no_dispatch_per_slot.no_ops_from_frontend de_no_dispatch_per_slot.backend_stalls \
          de_no_dispatch_per_slot.smt_contention ex_no_retire.not_complete \
          ex_no_retire.load_not_complete ex_ret_ucode_ops ex_ret_brn_misp \
          resyncs_or_nc_redirects bp_redirects.resync fp_ret_sse_avx_ops.all \
          fp_ops_retired_by_width.all fp_ops_retired_by_width.scalar_uops \
          fp_ops_retired_by_width.pack_128_uops fp_ops_retired_by_width.pack_256_uops \
          fp_ops_retired_by_width.pack_512_uops fp_ops_retired_by_type.all \
          ls_any_fills_from_sys.dram_io_all ls_dmnd_fills_from_sys.dram_io_far l2_cache_req_stat.ic_dc_miss_in_l2; do
    printf '%-56s %s\n' "$ev" "$(perf stat -vv -e "$ev" -- true 2>&1 | awk '$1=="config"{print $2; exit}' | grep . || echo 'unknown to perf')"
done
printf '\n$ perf list --details metricgroup PipelineL1 PipelineL2 (formulas)\n'
perf list --details metric 2>/dev/null | grep -B1 -A6 -E "^\s+(total_dispatch_slots|frontend_bound|bad_speculation|backend_bound|retiring|smt_contention)[a-z_]*\s*$" | head -150
printf '\n$ perf list --details > %s (full event and metric list, sent separately)\n' "${OUT%.txt}_perflist.txt"
perf list --details > "${OUT%.txt}_perflist.txt" 2>&1

sec "MPI toolchain"
for c in mpicc mpifort mpirun srun; do printf '%-8s %s\n' "$c" "$(command -v $c || echo 'not found')"; done
run mpirun --version
command -v mpicc >/dev/null && run mpicc --version
} > "$OUT" 2>&1

echo "Wrote $OUT ($(wc -l < "$OUT") lines) and ${OUT%.txt}_perflist.txt. Please send both back."
