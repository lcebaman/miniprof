#!/usr/bin/env bash
# pmu_probe_node.sh — what the P5 node-level (socket-wide) counters need:
# the AMD data fabric (amd_df), memory controller (amd_umc_*) and L3
# (amd_l3) PMUs, RAPL energy, and whether this user may read them.
#
# These counters are not per thread: perf can only open them system-wide
# (-a, or -C <cpu> on the PMU's cpumask), which perf_event_paranoid > 0
# forbids unless the process has CAP_PERFMON (or CAP_SYS_ADMIN). This
# script records what is there and tries each kind of access as YOU, with
# a memory-streaming workload so bandwidth counts are non-zero.
#
# Run on a COMPUTE node, ideally exclusive:
#     srun -N1 -n1 --exclusive tools/pmu_probe_node.sh   # or ./tools/pmu_probe_node.sh
# Writes pmu_probe_node_<host>.txt. No root needed; nothing is changed;
# sudo is only queried with -n (never prompts).
#
#   --sudo   also run the socket-counter tests through `sudo -n perf`, to
#            see that they count and get perf's exact encodings (configs),
#            and test the capability route: a COPY of perf in a private
#            temp dir gets cap_perfmon and is run as you; the copy is
#            deleted at exit. Nothing else on the system changes.

SUDO=0; [ "${1:-}" = "--sudo" ] && SUDO=1
OUT="pmu_probe_node_$(hostname -s).txt"
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sec() { printf '\n==================== %s ====================\n' "$1"; }
run() { printf '\n$ %s\n' "$*"; timeout 60 "$@" 2>&1; }

chmod 755 "$TMP"
# ~2 s of DRAM streaming on one core (triad over 3 x 256 MiB)
cat > "$TMP/stream.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
  long n = 32L*1024*1024;                       /* 256 MiB per array */
  double *a=malloc(n*8),*b=malloc(n*8),*c=malloc(n*8);
  for(long i=0;i<n;i++){a[i]=0;b[i]=1;c[i]=2;}
  double t0=now(); int reps=0;
  while(now()-t0<2.0){ for(long i=0;i<n;i++) a[i]=b[i]+3.0*c[i]; reps++; }
  double dt=now()-t0;
  printf("stream: %d reps, %.2f s, ~%.2f GB/s moved (24 B/elem incl. write-allocate: 32)\n",
         reps, dt, reps*(double)n*24/dt*1e-9);
  return a[n/2] < 0;
}
C
$CC -O2 -o "$TMP/stream" "$TMP/stream.c" 2>&1 || echo "could not build the stream kernel with $CC"
S="$TMP/stream"

{
sec "system"
run date -u
run hostname
grep -h PRETTY_NAME /etc/os-release 2>/dev/null
run uname -r
lscpu | grep -E 'Model name|Socket|Core\(s\) per|Thread\(s\) per|NUMA node|L3'
command -v numactl >/dev/null && run numactl -H
echo; echo "NPS (NUMA nodes per socket): $(( $(ls -d /sys/devices/system/node/node* | wc -l) / $(lscpu | awk -F: '/Socket\(s\)/{gsub(/ /,"",$2);print $2}') ))"

sec "access"
for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/nmi_watchdog; do
  printf '%-45s %s\n' "$f" "$(cat "$f" 2>&1)"
done
run id
command -v getpcaps >/dev/null && run getpcaps $$
P=$(command -v perf)
echo "perf: $P"
command -v getcap >/dev/null && run getcap "$P"
run perf --version
printf '\n$ sudo -n true\n'; sudo -n true 2>&1 && echo "passwordless sudo: yes" || echo "passwordless sudo: no"

sec "node-level PMUs (sysfs)"
for d in /sys/bus/event_source/devices/*; do
  n=$(basename "$d")
  case $n in amd_df|amd_umc*|amd_l3|power|amd_iommu*|msr) ;; *) continue ;; esac
  printf '\n--- %s  type=%s  cpumask=%s\n' "$n" "$(cat "$d/type" 2>/dev/null)" "$(cat "$d/cpumask" 2>/dev/null)"
  [ -d "$d/format" ] && for f in "$d"/format/*; do printf '    format %-12s %s\n' "$(basename "$f")" "$(cat "$f")"; done
  [ -d "$d/events" ] && ls "$d/events" | grep -v '\.\(scale\|unit\)$' | sed 's/^/    event  /' | head -40
done
echo; echo "amd_umc PMUs (one per memory channel): $(ls -d /sys/bus/event_source/devices/amd_umc_* 2>/dev/null | wc -l)"

sec "perf event lists"
run perf list --no-desc pmu 2>/dev/null | grep -iE 'amd_df|amd_umc|amd_l3|power/|umc_|df_' | head -60
printf '\n$ perf list metricgroups / metrics (memory, data fabric, power)\n'
perf list --details metric 2>/dev/null | grep -iE -A3 '^\s*(umc|dram|df_|.*bandwidth|.*memory|.*nps|.*socket|l3_|.*power|.*energy)' | head -120

sec "RAPL energy"
for z in /sys/class/powercap/*/; do
  [ -f "$z/energy_uj" ] || continue
  printf '%-50s name=%-10s energy_uj: %s\n' "$z" "$(cat "$z/name" 2>/dev/null)" "$(cat "$z/energy_uj" 2>&1)"
done
run perf stat -a -e power/energy-pkg/ -- sleep 1
run perf stat -a -e power/energy-cores/ -- sleep 1

sec "workload alone"
run "$S"

sec "system-wide (-a) access as this user"
run perf stat -a -e cycles -- sleep 0.2
run perf stat -C 0 -e cycles -- sleep 0.2

sec "memory controller (amd_umc): DRAM read/write CAS"
# kernel 6.x names; on older kernels only raw config works: event 0x0a = CAS commands,
# rdwrmask 1 = read, 2 = write (amd_umc format)
U=$(ls -d /sys/bus/event_source/devices/amd_umc_* 2>/dev/null | head -1 | xargs -r basename)
if [ -n "$U" ]; then
  run perf stat -a -e "$U/umc_cas_cmd.rd/,$U/umc_cas_cmd.wr/" -- "$S"
  run perf stat -a -e "$U/event=0x0a,rdwrmask=0x1/,$U/event=0x0a,rdwrmask=0x2/" -- "$S"
  run perf stat -a -M umc_mem_read_bandwidth,umc_mem_write_bandwidth -- "$S"
else
  echo "no amd_umc PMU on this kernel"
fi

sec "data fabric (amd_df)"
if [ -d /sys/bus/event_source/devices/amd_df ]; then
  # whatever DF events/metrics this perf knows (names vary by kernel and generation)
  EV=$(perf list --no-desc 2>/dev/null | grep -oE 'amd_df/[a-z0-9_]+/' | sort -u | head -6 | paste -sd, -)
  if [ -n "$EV" ]; then run perf stat -a -e "$EV" -- "$S"; else echo "perf knows no named DF events"; fi
  for m in nps1_die_to_dram local_socket_inbound_data_bytes local_socket_outbound_data_bytes \
           dram_read_data_for_local_processor dram_write_data_for_local_processor; do
    perf list --no-desc metric 2>/dev/null | grep -qw "$m" && run perf stat -a -M "$m" -- "$S"
  done
else
  echo "no amd_df PMU"
fi

sec "L3 (amd_l3)"
if [ -d /sys/bus/event_source/devices/amd_l3 ]; then
  run perf stat -a -e amd_l3/event=0x04,umask=0xff/ -- "$S"
  run perf stat -a -M l3_read_miss_latency -- "$S"
fi

if [ $SUDO = 1 ]; then
  if ! sudo -n true 2>/dev/null; then
    sec "sudo"; echo "--sudo given but passwordless sudo is not available"
  else
    U0=$(ls -d /sys/bus/event_source/devices/amd_umc_* 2>/dev/null | head -1 | xargs -r basename)
    DF=$(perf list --no-desc 2>/dev/null | grep -oE 'amd_df/local_or_remote_socket_read_data_beats_dram_[0-9]+/' | sort -u | paste -sd, -)
    DFW=$(perf list --no-desc 2>/dev/null | grep -oE 'amd_df/local_or_remote_socket_write_data_beats_dram_[0-9]+/' | sort -u | paste -sd, -)

    sec "sudo: memory controller (all channels), named + metrics"
    UALL=$(for d in /sys/bus/event_source/devices/amd_umc_*; do n=$(basename $d); printf '%s/umc_cas_cmd.rd/,%s/umc_cas_cmd.wr/,' $n $n; done | sed 's/,$//')
    run sudo -n perf stat -a -e "$UALL" -- "$S"
    run sudo -n perf stat -a -M umc_mem_read_bandwidth,umc_mem_write_bandwidth,umc_data_bus_utilization -- "$S"

    sec "sudo: data fabric (whichever events and metrics this generation has)"
    # Zen 5: local_or_remote_socket_{read,write}_data_beats_dram_N, *_socket_inbound_data_beats_cfi_N
    # Zen 4: {local,remote}_processor_{read,write}_data_beats_csN, *_socket_inf{0,1}_inbound_data_beats_ccmN
    for pat in 'local_or_remote_socket_read_data_beats_dram_[0-9]+' 'local_or_remote_socket_write_data_beats_dram_[0-9]+' \
               'local_processor_read_data_beats_cs[0-9]+' 'local_processor_write_data_beats_cs[0-9]+' \
               'remote_processor_read_data_beats_cs[0-9]+'; do
      EV=$(perf list --no-desc 2>/dev/null | grep -oE "amd_df/$pat/" | sort -u | paste -sd, -)
      [ -n "$EV" ] && run sudo -n perf stat -a -e "$EV" -- "$S"
    done
    MET=""
    for m in dram_read_bandwidth_for_local_or_remote_socket dram_write_bandwidth_for_local_or_remote_socket \
             core_inbound_data_bandwidth_for_local_socket core_inbound_data_bandwidth_for_remote_socket \
             cross_socket_inbound_data_bandwidth_for_local_socket \
             dram_read_data_for_local_processor dram_read_data_for_remote_processor \
             dram_write_data_for_local_processor dram_write_data_for_remote_processor \
             local_socket_inbound_data_to_cpu remote_socket_inbound_data_to_cpu \
             local_socket_outbound_data_from_all_links; do
      perf list --no-desc metric 2>/dev/null | grep -qw "$m" && MET="$MET $m"
    done
    echo; echo "data-fabric metrics on this node:$MET"
    for m in $MET; do run sudo -n perf stat -a -M "$m" -- "$S"; done

    sec "sudo: remote-NUMA stream (data on the other socket)"
    if command -v numactl >/dev/null; then
      LAST=$(( $(ls -d /sys/devices/system/node/node* | wc -l) - 1 ))
      for m in $MET; do
        case $m in *remote*|*inbound*|*dram_read*) ;; *) continue ;; esac
        run sudo -n perf stat -a -M "$m" -- numactl --cpunodebind=0 --membind=$LAST "$S"
      done
    fi

    sec "sudo: data-fabric encodings"
    for pat in 'local_or_remote_socket_read_data_beats_dram_[0-9]+' 'local_or_remote_socket_write_data_beats_dram_[0-9]+' \
               'local_processor_read_data_beats_cs[0-9]+' 'local_processor_write_data_beats_cs[0-9]+' \
               'remote_processor_read_data_beats_cs[0-9]+' 'remote_processor_write_data_beats_cs[0-9]+' \
               'local_socket_inf0_inbound_data_beats_ccm[0-9]+' 'remote_socket_inf0_inbound_data_beats_ccm[0-9]+' \
               'local_socket_inbound_data_beats_cfi_[0-9]+' 'remote_socket_inbound_data_beats_cfi_[0-9]+' \
               'local_socket_outbound_data_beats_link[0-9]+'; do
      for ev in $(perf list --no-desc 2>/dev/null | grep -oE "amd_df/$pat/" | sort -uV); do
        printf '%-58s %s\n' "$ev" "$(sudo -n perf stat -vv -a -e "$ev" -- true 2>&1 | awk '$1=="config"{print $2; exit}')"
      done
    done

    sec "sudo: L3, energy"
    run sudo -n perf stat -a -e amd_l3/event=0x04,umask=0xff,enallcores=1,enallslices=1,threadmask=3/ -- "$S"
    run sudo -n perf stat -a -e power/energy-pkg/ -- "$S"
    run sudo -n cat /sys/class/powercap/intel-rapl:0/energy_uj

    sec "sudo: perf's encodings (perf_event_attr type/config)"
    for ev in "$U0/umc_cas_cmd.rd/" "$U0/umc_cas_cmd.wr/" \
              power/energy-pkg/; do
      [ -n "$ev" ] || continue
      printf '\n%s\n' "$ev"
      sudo -n perf stat -vv -a -e "$ev" -- true 2>&1 | grep -E '^\s+(type|config|config1)\s' | head -3
    done
    for m in umc_mem_read_bandwidth $MET; do
      printf '\n$ perf list --details metric | %s\n' "$m"
      perf list --details metric 2>/dev/null | grep -A12 -E "^\s+$m\s*$" | head -14
    done

    sec "capability route (copy of perf + cap_perfmon, run as you)"
    cp "$(command -v perf)" "$TMP/perf" && chmod 755 "$TMP/perf"
    findmnt -no FSTYPE,OPTIONS -T "$TMP"
    if sudo -n setcap cap_perfmon+ep "$TMP/perf" 2>&1; then
      run getcap "$TMP/perf"
      run "$TMP/perf" stat -a -e "$U0/umc_cas_cmd.rd/,$U0/umc_cas_cmd.wr/,power/energy-pkg/" -- "$S"
      run "$TMP/perf" stat -a -M umc_mem_read_bandwidth -- "$S"
    fi
    printf '\nsetcap on the repository filesystem (%s):\n' "$PWD"
    findmnt -no FSTYPE,OPTIONS -T "$PWD"
    # store AND use: a capability-holding copy of perf on this filesystem,
    # run as you (a mount with nosuid, or a filesystem without xattrs,
    # would store or honour nothing)
    G="./.cap_perf_$$"
    if cp "$(command -v perf)" "$G" 2>/dev/null && chmod 755 "$G" && sudo -n setcap cap_perfmon+ep "$G" 2>&1; then
      run getcap "$G"
      run "$G" stat -a -e "$U0/umc_cas_cmd.rd/,power/energy-pkg/" -- "$S"
    fi
    sudo -n rm -f "$G"
    sudo -n rm -f "$TMP/perf"
  fi
fi

sec "done"
echo "If the system-wide runs above say 'Permission denied' / 'Access to performance"
echo "monitoring ... is limited': socket counters need perf_event_paranoid <= 0, or"
echo "CAP_PERFMON on the profiler (e.g. setcap cap_perfmon+ep on a small helper)."
} > "$OUT" 2>&1

echo "wrote $OUT"
