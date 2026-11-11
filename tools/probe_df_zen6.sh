#!/usr/bin/env bash
# probe_df_zen6.sh — map the data-fabric DRAM and core-inbound counters on
# a new AMD generation (written for Zen 6 / Venice, where perf 5.14 still
# applies Turin's tables: 12 DRAM channels, 32 B per CFI beat).
#
# For each placement of one pinned core and its memory (numactl), runs a
# kernel that moves a KNOWN number of bytes and counts, system-wide:
#   - DRAM read / write beats for channel slots cs 0..31
#       event 0x1f + 0x40*(n%4) + 0x100*(n/4), umask 0xffe (rd) / 0xfff (wr)
#   - core-inbound beats from local / remote memory for CFI ports 0..15
#       event 0x41e + 0x40*(n%4) + 0x100*((n/4)%2) + n/8, umask 0x7fe / 0xbfe
# and prints counted bytes (64 B per beat) against the kernel's own count,
# so channel coverage and bytes per beat can be read off directly.
# Also records why the amd_umc PMUs do not count.
#
# Needs system-wide counters: perf_event_paranoid <= 0, or --sudo (sudo -n).
#     srun -N1 --exclusive tools/probe_df_zen6.sh [--sudo]
# Writes probe_df_<host>.txt. Changes nothing.

AS=""; [ "${1:-}" = "--sudo" ] && AS="sudo -n"
OUT="probe_df_$(hostname -s).txt"
CC=${CC:-gcc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT; chmod 755 "$TMP"

# rd: sum over 1 GiB (DRAM reads = bytes); wr: store over 1 GiB (writes =
# bytes, plus as many write-allocate reads). Init first, then wait until
# 0.6 s after start, run exactly 2 s; perf starts counting at 0.5 s (-D).
cat > "$TMP/k.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
  double t00=now(); int wr = argc>1 && strcmp(argv[1],"wr")==0;
  long n = 1L<<27;                                /* 1 GiB of doubles */
  double *a = aligned_alloc(64, n*8);
  for (long i=0;i<n;i++) a[i]=(double)(i&7);
  while (now()-t00 < 0.6) ;
  double t0=now(), s0=0,s1=0,s2=0,s3=0; long reps=0;
  while (now()-t0 < 2.0) {
    if (wr) { for (long i=0;i<n;i++) a[i]=(double)reps; }
    else    { for (long i=0;i<n;i+=4){s0+=a[i];s1+=a[i+1];s2+=a[i+2];s3+=a[i+3];} }
    reps++;
  }
  printf("KERNEL %s bytes=%.0f secs=%.3f GBps=%.2f chk=%g\n", wr?"wr":"rd",
         (double)reps*n*8, now()-t0, reps*(double)n*8/(now()-t0)*1e-9, s0+s1+s2+s3+a[n/3]);
  return 0;
}
C
$CC -O2 -fno-tree-loop-distribute-patterns -o "$TMP/k" "$TMP/k.c" || { echo "cannot build kernel"; exit 1; }

dram_ev() { printf 'amd_df/event=0x%x,umask=0x%x/' $((0x1f + 0x40*($1%4) + 0x100*($1/4))) "$2"; }
cfi_ev()  { printf 'amd_df/event=0x%x,umask=0x%x/' $((0x41e + 0x40*($1%4) + 0x100*(($1/4)%2) + $1/8)) "$2"; }

# measure <label> <numactl args> <kernel mode> <event list...>
# prints one line per event: label count running% ; then the kernel line
measure() {
  local lab=$1 na=$2 mode=$3; shift 3
  local evs; evs=$(IFS=,; echo "$*")
  $AS perf stat -a -x, -D 500 -e "$evs" -o "$TMP/p.csv" -- numactl $na "$TMP/k" "$mode" > "$TMP/k.out" 2>&1
  awk -F, -v L="$lab" '/^[0-9<]/ {printf "%-6s %-40s %15s %s\n", L, $3, $1, $5}' "$TMP/p.csv"
  sed "s/^/$lab   /" "$TMP/k.out"
}

{
echo "== system"; date -u; hostname; uname -r
grep -m3 -E "^(cpu family|model\s|model name)" /proc/cpuinfo
numactl -H 2>&1 | head -4
cat /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/nmi_watchdog
N1=$(numactl -H | awk '/^node 1 cpus:/{print $4; exit}'); N1=${N1:-128}
echo "first cpu of node 1: $N1"

echo; echo "== amd_df / amd_umc format"
for p in amd_df amd_umc_0; do
  echo "-- $p"; for f in /sys/bus/event_source/devices/$p/format/*; do echo "$(basename $f): $(cat $f)"; done
  echo "cpumask: $(cat /sys/bus/event_source/devices/$p/cpumask)"
done
ls -d /sys/bus/event_source/devices/amd_umc_* | wc -l | sed 's/^/amd_umc PMUs: /'
echo "-- kernel messages about the uncore PMUs (may need root)"
(dmesg 2>/dev/null || $AS dmesg 2>/dev/null) | grep -i -E "umc|uncore|amd_df|perf" | tail -20
echo "-- umc on its cpumask cpu, one event, verbose"
$AS perf stat -vv -C 0 -e amd_umc_0/event=0x0a,rdwrmask=0x1/ -- sleep 0.3 2>&1 | grep -v -E "^\s*$" | tail -25
echo "-- umc clock (event 0x00), all 16"
$AS perf stat -a -x, -e $(for u in $(seq 0 15); do printf 'amd_umc_%d/event=0x00/,' $u; done | sed 's/,$//') -- sleep 0.3 2>&1 | tail -18

for P in "N0:--physcpubind=0 --membind=0" "N1:--physcpubind=$N1 --membind=1" "X01:--physcpubind=0 --membind=1"; do
  lab=${P%%:*}; na=${P#*:}
  echo; echo "== placement $lab ($na): DRAM read beats cs 0..31 (rd kernel)"
  for b in 0 8 16 24; do measure "$lab" "$na" rd $(for n in $(seq $b $((b+7))); do dram_ev $n 0xffe; done); done
  echo; echo "== placement $lab: DRAM write beats cs 0..31 (wr kernel)"
  for b in 0 8 16 24; do measure "$lab" "$na" wr $(for n in $(seq $b $((b+7))); do dram_ev $n 0xfff; done); done
  echo; echo "== placement $lab: core-inbound beats, local (0x7fe) and remote (0xbfe), cfi 0..15 (rd kernel)"
  for um in 0x7fe 0xbfe; do for b in 0 8; do
    measure "$lab" "$na" rd $(for n in $(seq $b $((b+7))); do cfi_ev $n $um; done); done; done
done

echo; echo "== counter budget: 16 DF events at once (any <100% running = multiplexed)"
measure B "--physcpubind=0 --membind=0" rd $(for n in $(seq 0 15); do dram_ev $n 0xffe; done)
echo; echo "== energy"
$AS perf stat -a -e power/energy-pkg/ -- numactl --physcpubind=0 "$TMP/k" rd 2>&1 | grep -E "KERNEL|Joules|elapsed"
echo "== done"
} 2>&1 | tee "$OUT"

# summary: counted bytes (64 B/beat) vs the kernel's bytes, per placement and block
echo
echo "== summary (64 B per beat): counted / kernel bytes" | tee -a "$OUT"
awk '
  /^(N0|N1|X01) / && $2 ~ /^amd_df/ {
    lab=$1; ev=$2; c=$3+0
    if (ev ~ /umask=0xffe/) k="dram_rd"; else if (ev ~ /umask=0xfff/) k="dram_wr";
    else if (ev ~ /umask=0x7fe/) k="cfi_local"; else if (ev ~ /umask=0xbfe/) k="cfi_remote"; else next
    tot[lab" "k]+=c
    match(ev, /event=0x[0-9a-f]+/); e=substr(ev, RSTART+6, RLENGTH-6)
    if (c > 1e6) hit[lab" "k]=hit[lab" "k]" "e
  }
  /KERNEL/ { lab=$1; m=$3; split($4,b,"="); kb[lab" "m]=b[2]; nk[lab" "m]++ ; skb[lab" "m]+=b[2] }
  END {
    for (x in tot) {
      split(x, p, " "); m = (p[2]=="dram_wr") ? "wr" : "rd"
      # every perf run in a block moved about the same: average kernel bytes
      kbytes = skb[p[1]" "m] / nk[p[1]" "m]
      printf "%-4s %-10s %8.2f GB  = %5.1f%% of kernel  active:%s\n", p[1], p[2],
             tot[x]*64/1e9, 100*tot[x]*64/kbytes, hit[x]
    }
  }' "$OUT" | sort | tee -a "$OUT"
echo "wrote $OUT"
