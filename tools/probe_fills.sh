#!/usr/bin/env bash
# probe_fills.sh — which core events see a kernel's DRAM reads (AMD Zen 4/5/6).
#
# One pinned core runs kernels over 512 MiB arrays (far beyond L3), each
# moving a known number of bytes, under perf stat with:
#   ls_any_fills_from_sys.dram_io_all  L1D fills from DRAM (demand + L1 prefetch)
#   l2_pf_miss_l2_l3.l2_hwpf           L2-prefetcher requests that miss L2 and L3
#   l2_pf_miss_l2_l3.l1_dc_hwpf        L1-prefetcher requests that miss L2 and L3
#   l2_pf_miss_l2_l3.l1_dc_l2_hwpf     both
# and prints each, x 64 B, as % of the bytes the kernel reads from DRAM.
# Kernels: dot (read 2 arrays), sum (read 1), daxpy (read 2, write 1),
# copy (read 1, write 1), stride (read 1 element per 64 B line, no
# streaming prefetch pattern beyond next-line).
# User-space events of our own process: works at perf_event_paranoid 2.
#     srun -N1 --exclusive tools/probe_fills.sh
set -u
OUT="probe_fills_$(hostname -s).txt"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/k.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    long n = 1L << 26;                       /* 64 Mi doubles = 512 MiB per array */
    int reps = argc > 2 ? atoi(argv[2]) : 4;
    double *x = aligned_alloc(64, n * 8), *y = aligned_alloc(64, n * 8);
    for (long i = 0; i < n; i++) { x[i] = 1e-9 * i; y[i] = 1.0; }
    double s = 0, rd = 0;
    const char *k = argv[1];
    for (int r = 0; r < reps; r++) {
        if (!strcmp(k, "dot"))      { for (long i = 0; i < n; i++) s += x[i] * y[i]; rd += 16.0 * n; }
        else if (!strcmp(k, "sum")) { for (long i = 0; i < n; i++) s += x[i]; rd += 8.0 * n; }
        else if (!strcmp(k, "daxpy")) { for (long i = 0; i < n; i++) y[i] += 1.000001 * x[i]; rd += 16.0 * n; }
        else if (!strcmp(k, "copy")) { for (long i = 0; i < n; i++) y[i] = x[i]; rd += 16.0 * n; } /* x + y write-allocate */
        else if (!strcmp(k, "stride")) { for (long i = 0; i < n; i += 8) s += x[i]; rd += 8.0 * n; }
    }
    printf("READ_BYTES %.0f chk %g\n", rd, s + y[n / 2]);
    return 0;
}
C
${CC:-gcc} -O2 -fno-tree-loop-distribute-patterns -o "$TMP/k" "$TMP/k.c" || exit 1
EV=ls_any_fills_from_sys.dram_io_all,l2_pf_miss_l2_l3.l2_hwpf,l2_pf_miss_l2_l3.l1_dc_hwpf,l2_pf_miss_l2_l3.l1_dc_l2_hwpf
{
echo "== $(hostname) $(date -u)"; grep -m1 "model name" /proc/cpuinfo; uname -r
echo "kernel  event                                   counted GB   % of DRAM reads   (running%)"
for k in dot sum daxpy copy stride; do
    # init pass alone, to subtract: the arrays' first touch is in both runs
    perf stat -x';' -e "$EV:u" -o "$TMP/p0" -- taskset -c 2 "$TMP/k" "$k" 0 > /dev/null
    perf stat -x';' -e "$EV:u" -o "$TMP/p1" -- taskset -c 2 "$TMP/k" "$k" 4 > "$TMP/out"
    rb=$(awk '/READ_BYTES/{print $2}' "$TMP/out")
    paste -d';' <(grep -v '^#' "$TMP/p0" | grep -v '^$') <(grep -v '^#' "$TMP/p1" | grep -v '^$') |
    awk -F';' -v k="$k" -v rb="$rb" 'NF > 8 { c = $(NF/2 + 1) - $1; printf "%-7s %-40s %9.2f   %6.1f%%   (%s)\n", k, $3, c*64/1e9, 100*c*64/rb, $(NF/2 + 5) }'
done
echo "== done"
} 2>&1 | tee "$OUT"
echo "wrote $OUT"
