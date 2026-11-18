#!/usr/bin/env bash
# probe_intel.sh — check, on an Intel Xeon node (Granite Rapids first), the
# events miniprof's Intel support uses, against kernels with a known
# answer:
#   - core: FP_ARITH_INST_RETIRED by width (FLOPs), OCR.READS_TO_CORE
#     DRAM / REMOTE_DRAM (offcore_rsp), L2_RQSTS.MISS, the vector-width
#     umasks, cycles/instructions; how many counters fit;
#   - top-down: slots + topdown-* metric events in one user-space group,
#     opened exactly as miniprof opens it (perf_event_open, exclude_kernel,
#     PERF_FORMAT_GROUP), and whether rdpmc can read them;
#   - node: uncore IMC PMU names and format, CAS_COUNT_SCH0/1 RD/WR
#     against a known byte count, RAPL energy.
#
# Run on a COMPUTE node, ideally exclusive, from the repo root:
#     ./tools/probe_intel.sh            # core and top-down
#     ./tools/probe_intel.sh --sudo     # also uncore IMC and RAPL (need root
#                                       # or perf_event_paranoid <= 0)
# Writes probe_intel_<host>.txt; send it back. Nothing is installed.

AS=""
[ "${1:-}" = "--sudo" ] && AS="sudo"
OUT="probe_intel_$(hostname -s).txt"
CC=${CC:-gcc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SYS=/sys/bus/event_source/devices

sec() { printf '\n==================== %s ====================\n' "$1"; }
run() { printf '\n$ %s\n' "$*"; "$@" 2>&1; }

# ---------------------------------------------------------------- kernels
cat > "$TMP/k.c" <<'EOF'
/* Kernels with a known answer. Single thread, pinned by the caller.
 *   fma512d N R : 12 AVX-512 FP64 FMA chains, R*N*12 FMAs = R*N*12*16 FLOPs
 *   fma256s N R : AVX2 FP32 FMA chains,        R*N*12*16 FLOPs (8 lanes x 2)
 *   scalard N R : scalar FP64 FMA chains,      R*N*12*2 FLOPs
 *   read  MiB R : sum of a MiB array, R times: R*MiB*2^20 bytes read
 *   write MiB R : store to a MiB array, R times (plus the first touch)
 * Prints the expected FLOPs or bytes. */
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#define CH 12
/* asm volatile: keeps GCC from treating the kernels as pure and calling
 * them once for the R repetitions */
__attribute__((target("avx512f"),noinline)) static double fma512d(long n){
  __asm__ volatile("");
  __m512d a[CH],x=_mm512_set1_pd(1.0000001),y=_mm512_set1_pd(1e-9);
  for(int i=0;i<CH;i++)a[i]=_mm512_set1_pd(i);
  for(long k=0;k<n;k++) for(int i=0;i<CH;i++) a[i]=_mm512_fmadd_pd(a[i],x,y);
  double s=0; for(int i=0;i<CH;i++) s+=_mm512_reduce_add_pd(a[i]); return s;}
__attribute__((target("avx2,fma"),noinline)) static double fma256s(long n){
  __asm__ volatile("");
  __m256 a[CH],x=_mm256_set1_ps(1.0000001f),y=_mm256_set1_ps(1e-9f);
  for(int i=0;i<CH;i++)a[i]=_mm256_set1_ps(i);
  for(long k=0;k<n;k++) for(int i=0;i<CH;i++) a[i]=_mm256_fmadd_ps(a[i],x,y);
  float t[8]; double s=0; for(int i=0;i<CH;i++){_mm256_storeu_ps(t,a[i]); for(int j=0;j<8;j++) s+=t[j];} return s;}
__attribute__((target("fma"),noinline)) static double scalard(long n){
  __asm__ volatile("");
  __m128d a[CH],x=_mm_set_sd(1.0000001),y=_mm_set_sd(1e-9);
  for(int i=0;i<CH;i++)a[i]=_mm_set_sd(i);
  for(long k=0;k<n;k++) for(int i=0;i<CH;i++) a[i]=_mm_fmadd_sd(a[i],x,y);
  double s=0; for(int i=0;i<CH;i++) s+=_mm_cvtsd_f64(a[i]); return s;}
int main(int c,char**v){
  if(c<4){fprintf(stderr,"usage: k fma512d|fma256s|scalard|read|write N R\n");return 2;}
  long n=atol(v[2]),r=atol(v[3]); double s=0,t0=now();
  if(!strcmp(v[1],"fma512d")){for(long i=0;i<r;i++)s+=fma512d(n);printf("expected_flops %ld\n",r*n*CH*16);}
  else if(!strcmp(v[1],"fma256s")){for(long i=0;i<r;i++)s+=fma256s(n);printf("expected_flops %ld\n",r*n*CH*16);}
  else if(!strcmp(v[1],"scalard")){for(long i=0;i<r;i++)s+=scalard(n);printf("expected_flops %ld\n",r*n*CH*2);}
  else { long e=n*(1L<<20)/8; double*a=aligned_alloc(64,e*8); for(long i=0;i<e;i++)a[i]=1;
    t0=now();
    if(!strcmp(v[1],"read")){for(long k=0;k<r;k++){double s0=0,s1=0,s2=0,s3=0;for(long i=0;i<e;i+=4){s0+=a[i];s1+=a[i+1];s2+=a[i+2];s3+=a[i+3];}s+=s0+s1+s2+s3;}}
    else {for(long k=0;k<r;k++){for(long i=0;i<e;i++)a[i]=k;__asm__ volatile(""::"r"(a):"memory");}}
    printf("expected_bytes %ld (%.1f GB/s)\n",r*e*8,r*e*8/(now()-t0)*1e-9); }
  printf("seconds %.3f sink %g\n",now()-t0,s); return 0;}
EOF

# ------------------------------------------------- top-down group, as miniprof opens it
cat > "$TMP/td.c" <<'EOF'
/* Open slots + 8 topdown metric events (+ INT_MISC.UOP_DROPPING) as one
 * user-space group, the way miniprof does, run a loop, read the group
 * with read() and report whether the mmap pages allow rdpmc. Also prints
 * CPUID leaf 0xA (counters per thread). */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static int ev(uint64_t cfg,int leader,int excl){struct perf_event_attr a;memset(&a,0,sizeof a);a.size=sizeof a;
  a.type=PERF_TYPE_RAW;a.config=cfg;a.disabled=leader<0;a.exclude_kernel=excl;a.exclude_hv=excl;
  a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
  return (int)syscall(__NR_perf_event_open,&a,0,-1,leader,0);}
int main(void){
  unsigned A,B,C,D; __cpuid(0xa,A,B,C,D);
  printf("cpuid 0xA: version %u, GP counters/thread %u (width %u), fixed counters %u\n",A&0xff,(A>>8)&0xff,(A>>16)&0xff,D&0x1f);
  const char*nm[]={"slots","topdown-retiring","topdown-bad-spec","topdown-fe-bound","topdown-be-bound",
    "topdown-heavy-ops","topdown-br-mispredict","topdown-fetch-lat","topdown-mem-bound","INT_MISC.UOP_DROPPING","cycles(0x3c)"};
  uint64_t cfg[]={0x0400,0x8000,0x8100,0x8200,0x8300,0x8400,0x8500,0x8600,0x8700,0x10ad,0x003c};
  for(int excl=1;excl>=0;excl--){
    int n=11,fd[11]; printf("\n-- exclude_kernel=%d\n",excl);
    for(int i=0;i<n;i++){fd[i]=ev(cfg[i],i?fd[0]:-1,excl); if(fd[i]<0){printf("open %s: %s\n",nm[i],strerror(errno));n=i;break;}}
    if(n==0) continue;
    struct perf_event_mmap_page*pg=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd[1<n?1:0],0);
    ioctl(fd[0],PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP); ioctl(fd[0],PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP);
    volatile double x=1; for(long i=0;i<200000000;i++) x=x*1.0000001+1e-9;
    if(pg!=MAP_FAILED) printf("mmap page of %s: cap_user_rdpmc=%u index=%u\n",nm[1<n?1:0],pg->cap_user_rdpmc,pg->index);
    ioctl(fd[0],PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP);
    uint64_t b[3+16]; ssize_t r=read(fd[0],b,sizeof b);
    printf("read %zd bytes: nr=%lu enabled=%lu running=%lu\n",r,b[0],b[1],b[2]);
    double tot=0; for(int i=1;i<=4&&i<n;i++) tot+=b[3+i];
    for(int i=0;i<n;i++) printf("  %-24s %16lu  %5.1f%%\n",nm[i],b[3+i],i>0&&i<9&&tot>0?100.0*b[3+i]/tot:0.0);
    printf("  sum of the 4 level-1 metrics / slots = %.3f\n",b[3]?tot/b[3]:0);
    for(int i=0;i<n;i++) close(fd[i]);
  }
  return 0;}
EOF

{
sec "system"
run date -u; run hostname; run uname -r
grep -h PRETTY_NAME /etc/os-release 2>/dev/null
awk '/^processor/{n++} n==1' /proc/cpuinfo | grep -E '^(vendor_id|cpu family|model|model name|stepping|microcode)'
lscpu | grep -E 'Socket|Core|Thread|NUMA node|Model name|L3'
for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/nmi_watchdog $SYS/cpu/rdpmc; do
  printf '%-48s %s\n' "$f" "$(cat "$f" 2>&1)"; done
run perf --version
echo "cpu PMU caps:"; for f in $SYS/cpu/caps/*; do printf '  %s=%s\n' "$(basename "$f")" "$(cat "$f")"; done 2>/dev/null
echo "cpu PMU format:"; for f in $SYS/cpu/format/*; do printf '  %s=%s\n' "$(basename "$f")" "$(cat "$f")"; done
echo "topdown events in sysfs:"; for f in $SYS/cpu/events/slots $SYS/cpu/events/topdown-*; do
  [ -e "$f" ] && printf '  %s=%s\n' "$(basename "$f")" "$(cat "$f")"; done

sec "build"
run $CC -O2 -o "$TMP/k" "$TMP/k.c" || { echo "cannot build kernels"; exit 1; }
run $CC -O2 -o "$TMP/td" "$TMP/td.c"
P="taskset -c 2"

sec "FLOPs: FP_ARITH_INST_RETIRED as miniprof weights them (SCALAR x1, 128D x2, 4_FLOPS x4, 8_FLOPS x8, 512S x16)"
FL="cpu/event=0xc7,umask=0x03,name=scalar/u,cpu/event=0xc7,umask=0x04,name=p128d/u,cpu/event=0xc7,umask=0x18,name=f4/u,cpu/event=0xc7,umask=0x60,name=f8/u,cpu/event=0xc7,umask=0x80,name=p512s/u,cycles:u,instructions:u"
for k in fma512d fma256s scalard; do
  run perf stat -x, -e "$FL" -- $P "$TMP/k" $k 2000000 10
done
echo; echo "weighted = scalar + 2*p128d + 4*f4 + 8*f8 + 16*p512s should equal expected_flops"

sec "vector widths (full level): SCALAR 0x03, 128 0x0c, 256 0x30, 512 0xc0"
VW="cpu/event=0xc7,umask=0x03,name=w_scalar/u,cpu/event=0xc7,umask=0x0c,name=w_128/u,cpu/event=0xc7,umask=0x30,name=w_256/u,cpu/event=0xc7,umask=0xc0,name=w_512/u,cycles:u"
run perf stat -x, -e "$VW" -- $P "$TMP/k" fma256s 2000000 5

sec "the base group as miniprof opens it (5 FLOP + DRAM + remote DRAM + L2 miss): does it all fit?"
BASE="{cycles:u,instructions:u,cpu/event=0xc7,umask=0x03/u,cpu/event=0xc7,umask=0x04/u,cpu/event=0xc7,umask=0x18/u,cpu/event=0xc7,umask=0x60/u,cpu/event=0xc7,umask=0x80/u,cpu/event=0x2a,umask=0x01,offcore_rsp=0x73C004477,name=ocr_dram/u,cpu/event=0x2b,umask=0x01,offcore_rsp=0x730004477,name=ocr_remote_dram/u,cpu/event=0x24,umask=0x3f,name=l2_miss/u}"
run perf stat -x, -e "$BASE" -- $P "$TMP/k" read 2048 5

sec "DRAM reads seen by the core: OCR.READS_TO_CORE.DRAM vs a known byte count"
OCR="cpu/event=0x2a,umask=0x01,offcore_rsp=0x73C004477,name=ocr_dram/u,cpu/event=0x2b,umask=0x01,offcore_rsp=0x730004477,name=ocr_remote_dram/u,cpu/event=0x2a,umask=0x01,offcore_rsp=0x104004477,name=ocr_local_dram/u"
for cpu_mem in "2 0" "2 1"; do
  set -- $cpu_mem
  echo; echo "--- core on CPU $1, memory on NUMA node $2 (remote when node $2 is the other socket)"
  run perf stat -x, -e "$OCR" -- numactl --physcpubind=$1 --membind=$2 "$TMP/k" read 2048 5
done
echo; echo "lines x 64 should be close to expected_bytes (plus the first touch)"
run perf stat -x, -e OCR.READS_TO_CORE.DRAM:u -- $P "$TMP/k" read 2048 5

sec "top-down: perf's reference"
run perf stat -M TopdownL1,TopdownL2 -- $P "$TMP/k" fma512d 2000000 10
run perf stat -M TopdownL1,TopdownL2 -- $P "$TMP/k" read 2048 3
run perf stat -e '{slots,topdown-retiring,topdown-bad-spec,topdown-fe-bound,topdown-be-bound,topdown-heavy-ops,topdown-br-mispredict,topdown-fetch-lat,topdown-mem-bound}:u' -- $P "$TMP/k" read 2048 3

sec "top-down group opened like miniprof (perf_event_open, PERF_FORMAT_GROUP)"
run $P "$TMP/td"

sec "uncore PMUs"
ls -d $SYS/uncore_* 2>/dev/null | xargs -rn1 basename | sed 's/_[0-9]*$//' | sort | uniq -c
for d in $(ls -d $SYS/uncore_imc* $SYS/uncore_type_* 2>/dev/null | head -40); do
  printf '%-40s type=%s cpumask=%s\n' "$(basename $d)" "$(cat $d/type)" "$(cat $d/cpumask 2>/dev/null)"; done
D=$(ls -d $SYS/uncore_imc_[0-9]* 2>/dev/null | head -1)
[ -n "$D" ] && { echo "format of $(basename $D):"; for f in $D/format/*; do printf '  %s=%s\n' "$(basename $f)" "$(cat $f)"; done; ls $D/events 2>/dev/null | head -20; }
run perf list --no-desc 2>/dev/null | grep -iE 'unc_m_cas|cas_count|memory_bandwidth' | head -20
for d in $SYS/power; do [ -d $d ] && { echo "power PMU:"; ls $d/events; cat $d/cpumask; }; done

if [ -n "$AS" ]; then
  sec "uncore IMC CAS counts vs known bytes (sudo)"
  IMCS=$(ls -d $SYS/uncore_imc_[0-9]* 2>/dev/null | xargs -rn1 basename)
  if [ -n "$IMCS" ]; then
    EV=$(for n in $IMCS; do printf '%s/event=0x05,umask=0xcf/,%s/event=0x06,umask=0xcf/,%s/event=0x05,umask=0xf0/,%s/event=0x06,umask=0xf0/,' $n $n $n $n; done | sed 's/,$//')
    for k in read write; do
      echo; echo "--- $k 2048 MiB x5 on CPU 2, local memory"
      run $AS perf stat -a --no-merge -x, -e "$EV" -- numactl --physcpubind=2 --localalloc "$TMP/k" $k 2048 5
    done
    run $AS perf stat -a -M memory_bandwidth_read,memory_bandwidth_write -- numactl --physcpubind=2 --localalloc "$TMP/k" read 2048 5
  else
    echo "no uncore_imc_N PMUs: listing every uncore PMU with its events"
    for d in $SYS/uncore_*; do echo "$(basename $d): $(ls $d/events 2>/dev/null | head -8 | tr '\n' ' ')"; done
  fi
  sec "RAPL energy (sudo)"
  run $AS perf stat -a -e power/energy-pkg/,power/energy-ram/ -- sleep 2
fi
} > "$OUT" 2>&1
echo "wrote $OUT"
