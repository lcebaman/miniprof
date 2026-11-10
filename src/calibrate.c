/*
 * calibrate.c — miniprof-calibrate: measure what a node can do (P6)
 *
 * Run once per node type, on an exclusive node:
 *     srun -N1 --exclusive ./miniprof-calibrate -o calib/
 * Writes calib/calibration_<host>.json. Snapshot runs pointed at it
 * (MINIPROF_CALIBRATION=calib/ or a file) report bandwidth and FLOP rates
 * as a percentage of these measured ceilings, and a roofline position.
 *
 * Measures, with one thread pinned to each physical core it may use
 * (SMT siblings left idle; restrict with taskset/srun to calibrate less):
 *   - DRAM bandwidth, best of several repetitions, per socket (threads and
 *     first-touched memory on that socket) and for the whole node:
 *       read      sum of an array: 8 B/element, no writes
 *       triad     a = b + s*c: STREAM counts 24 B/element; DRAM moves
 *                 32 (a is read before it is written: write-allocate).
 *                 Both are reported; the snapshot's DRAM counters see 32.
 *   - peak FLOP rate per vector width and precision: independent FMA
 *     chains in registers (no memory), all cores at once (the clock under
 *     full load) and one core (the boost clock). FMA = 2 FLOPs.
 *
 * No MPI. Pure pthreads + intrinsics with per-function target attributes,
 * so one binary runs on any x86-64 and skips widths the CPU lacks.
 */

#define _GNU_SOURCE
#include "json_writer.h"
#include "sysinfo.h"

#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CORES 1024

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── topology: one CPU per physical core in our affinity mask ────────── */

typedef struct { int cpu, socket; } core_t;
static core_t g_cores[MAX_CORES];
static int    g_ncores = 0, g_nsockets = 0;

static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}

static void topology(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(set), &set);
    static int seen_core[64][4096];   /* [socket][core_id] */
    char p[256];
    for (int c = 0; c < CPU_SETSIZE && g_ncores < MAX_CORES; c++) {
        if (!CPU_ISSET(c, &set)) continue;
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", c);
        int s = read_int(p);
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/topology/core_id", c);
        int k = read_int(p);
        if (s < 0) s = 0;
        if (k < 0) k = c;
        if (s >= 64 || k >= 4096 || seen_core[s][k]) continue;   /* SMT sibling */
        seen_core[s][k] = 1;
        g_cores[g_ncores].cpu = c;
        g_cores[g_ncores].socket = s;
        g_ncores++;
        if (s + 1 > g_nsockets) g_nsockets = s + 1;
    }
}

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* ── thread team ─────────────────────────────────────────────────────── */

typedef struct team team_t;
typedef double (*work_fn)(team_t *t, int id);   /* returns work units (bytes or FLOPs) */

struct team {
    int               n;
    int               cpu[MAX_CORES];
    pthread_barrier_t bar;
    work_fn           fn;
    double            units[MAX_CORES];
    double            t0, t1;
    /* bandwidth state */
    long              n_per;               /* elements per thread */
    double           *a[MAX_CORES], *b[MAX_CORES], *c[MAX_CORES];
    int               kernel;              /* 0 read, 1 triad */
    int               reps;
    double            best;                /* best seconds per rep (bandwidth) */
    /* FLOP state */
    int               fkernel;
    double            seconds;
};

typedef struct { team_t *t; int id; } arg_t;

static void *thread_main(void *p)
{
    arg_t *a = p;
    pin(a->t->cpu[a->id]);
    a->t->units[a->id] = a->t->fn(a->t, a->id);
    return NULL;
}

static void run_team(team_t *t)
{
    pthread_t th[MAX_CORES];
    arg_t args[MAX_CORES];
    pthread_barrier_init(&t->bar, NULL, (unsigned)t->n);
    for (int i = 0; i < t->n; i++) {
        args[i].t = t; args[i].id = i;
        pthread_create(&th[i], NULL, thread_main, &args[i]);
    }
    for (int i = 0; i < t->n; i++) pthread_join(th[i], NULL);
    pthread_barrier_destroy(&t->bar);
}

/* ── DRAM bandwidth ──────────────────────────────────────────────────── */

static volatile double g_sink;

static double bw_work(team_t *t, int id)
{
    long n = t->n_per;
    /* first touch by the owning thread: memory lands on its NUMA node */
    double *a = aligned_alloc(64, (size_t)n * sizeof(double) + 64);
    double *b = aligned_alloc(64, (size_t)n * sizeof(double) + 64);
    double *c = aligned_alloc(64, (size_t)n * sizeof(double) + 64);
    for (long i = 0; i < n; i++) { a[i] = 0.0; b[i] = 1.0; c[i] = 2.0; }
    double sum = 0;
    for (int r = 0; r < t->reps; r++) {
        pthread_barrier_wait(&t->bar);
        if (id == 0) t->t0 = now();
        pthread_barrier_wait(&t->bar);
        if (t->kernel == 0) {
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            for (long i = 0; i < n; i += 4) { s0 += b[i]; s1 += b[i + 1]; s2 += b[i + 2]; s3 += b[i + 3]; }
            sum += s0 + s1 + s2 + s3;
        } else {
            for (long i = 0; i < n; i++) a[i] = b[i] + 3.0 * c[i];
            __asm__ volatile("" ::: "memory");
        }
        pthread_barrier_wait(&t->bar);
        if (id == 0) {
            double dt = now() - t->t0;
            if (r > 0 && (t->best == 0 || dt < t->best)) t->best = dt;   /* rep 0 warms up */
        }
    }
    g_sink += sum + a[n / 2];
    free(a); free(b); free(c);
    return 0;
}

/* bytes per element as DRAM sees it: read 8; triad 32 (24 STREAM) */
static double bandwidth(const int *cpus, int n, long bytes_per_array_total, int kernel, int reps)
{
    static team_t t;
    memset(&t, 0, sizeof(t));
    t.n = n;
    memcpy(t.cpu, cpus, sizeof(int) * (size_t)n);
    t.fn = bw_work;
    t.kernel = kernel;
    t.reps = reps;
    t.n_per = (bytes_per_array_total / 8 / n) & ~7L;
    run_team(&t);
    double elems = (double)t.n_per * n;
    return elems * (kernel == 0 ? 8.0 : 32.0) / t.best * 1e-9;   /* GB/s, DRAM view */
}

/* ── FLOP kernels: 12 independent FMA chains per thread ──────────────────
 * 12 > the 8 in flight a 2-pipe, 4-cycle FMA unit needs, and 12 + 2
 * constants fit the 16 ymm registers of AVX2 (16 chains spilled). No
 * barrier inside the loop: FMA chains cannot be folded without fast-math,
 * and the results are consumed, so the compiler keeps them in registers
 * (an asm "+m" barrier forced a store/load per chain: ~60% of peak). */

#define CHAINS 12
#define FLOP_SECONDS 1.0

#define FMA_KERNEL(NAME, TGT, VT, SET1, FMA, LANES)                               \
__attribute__((target(TGT), noinline)) static double NAME(double seconds)        \
{                                                                                 \
    VT x[CHAINS];                                                                 \
    VT m = SET1(0.9999999), c = SET1(1e-9);                                       \
    for (int k = 0; k < CHAINS; k++) x[k] = SET1(1.0 + k);                        \
    long iters = 0;                                                               \
    double t0 = now();                                                            \
    do {                                                                          \
        for (int r = 0; r < 4096; r++) {                                          \
            for (int k = 0; k < CHAINS; k++) x[k] = FMA(x[k], m, c);              \
        }                                                                         \
        iters += 4096;                                                            \
    } while (now() - t0 < seconds);                                               \
    VT s = x[0];                                                                  \
    for (int k = 1; k < CHAINS; k++) s = FMA(s, m, x[k]);                         \
    double out[16];                                                               \
    memcpy(out, &s, sizeof(s) < sizeof(out) ? sizeof(s) : sizeof(out));          \
    g_sink += out[0];                                                             \
    return (double)iters * CHAINS * (LANES) * 2.0;                                \
}

FMA_KERNEL(fma_sd,  "fma",     __m128d, _mm_set1_pd,    _mm_fmadd_sd,    1)
FMA_KERNEL(fma_pd2, "fma",     __m128d, _mm_set1_pd,    _mm_fmadd_pd,    2)
FMA_KERNEL(fma_pd4, "avx2,fma", __m256d, _mm256_set1_pd, _mm256_fmadd_pd, 4)
FMA_KERNEL(fma_pd8, "avx512f", __m512d, _mm512_set1_pd, _mm512_fmadd_pd, 8)
FMA_KERNEL(fma_ss,  "fma",     __m128,  _mm_set1_ps,    _mm_fmadd_ss,    1)
FMA_KERNEL(fma_ps4, "fma",     __m128,  _mm_set1_ps,    _mm_fmadd_ps,    4)
FMA_KERNEL(fma_ps8, "avx2,fma", __m256,  _mm256_set1_ps, _mm256_fmadd_ps, 8)
FMA_KERNEL(fma_ps16,"avx512f", __m512,  _mm512_set1_ps, _mm512_fmadd_ps, 16)

typedef struct { const char *key, *isa; double (*fn)(double); } fkern_t;
static const fkern_t k_fk[] = {
    { "fp64_scalar", "fma",     fma_sd   }, { "fp64_128", "fma",     fma_pd2 },
    { "fp64_256",    "avx2",    fma_pd4  }, { "fp64_512", "avx512f", fma_pd8 },
    { "fp32_scalar", "fma",     fma_ss   }, { "fp32_128", "fma",     fma_ps4 },
    { "fp32_256",    "avx2",    fma_ps8  }, { "fp32_512", "avx512f", fma_ps16 },
};
#define NFK (int)(sizeof(k_fk) / sizeof(k_fk[0]))

static int isa_ok(const char *isa)
{
    __builtin_cpu_init();
    if (strcmp(isa, "fma") == 0) return __builtin_cpu_supports("fma");
    if (strcmp(isa, "avx2") == 0) return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (strcmp(isa, "avx512f") == 0) return __builtin_cpu_supports("avx512f");
    return 0;
}

static double flop_work(team_t *t, int id)
{
    (void)id;
    pthread_barrier_wait(&t->bar);
    return k_fk[t->fkernel].fn(t->seconds);
}

static double flops(const int *cpus, int n, int k, double seconds)
{
    static team_t t;
    memset(&t, 0, sizeof(t));
    t.n = n;
    memcpy(t.cpu, cpus, sizeof(int) * (size_t)n);
    t.fn = flop_work;
    t.fkernel = k;
    t.seconds = seconds;
    double t0 = now();
    run_team(&t);
    double dt = now() - t0, total = 0;
    for (int i = 0; i < n; i++) total += t.units[i];
    return total / dt * 1e-9;   /* GFLOP/s */
}

/* ── main ────────────────────────────────────────────────────────────── */

static long l3_bytes_total(void)
{
    /* sum of distinct L3 instances is awkward; use the per-CPU size times
     * the number of CCX-sized groups via sysfs shared_cpu_list counts */
    char p[256], b[64];
    long total = 0;
    static int seen[MAX_CORES * 2];
    for (int i = 0; i < g_ncores; i++) {
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cache/index3/id", g_cores[i].cpu);
        int id = read_int(p);
        if (id < 0 || id >= MAX_CORES * 2 || seen[id]) continue;
        seen[id] = 1;
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cache/index3/size", g_cores[i].cpu);
        FILE *f = fopen(p, "r");
        if (f && fgets(b, sizeof(b), f)) total += atol(b) * 1024L;   /* "32768K" */
        if (f) fclose(f);
    }
    return total;
}

int main(int argc, char **argv)
{
    const char *out = ".";
    int quick = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--quick") == 0) quick = 1;
        else {
            fprintf(stderr, "usage: miniprof-calibrate [-o DIR|FILE.json] [--quick]\n"
                            "  --quick  small arrays, short kernels: tests the tool, not the node\n");
            return 2;
        }
    }
    sysinfo_node_t node;
    sysinfo_collect_node(&node);
    topology();
    if (g_ncores == 0) { fprintf(stderr, "no CPUs in the affinity mask\n"); return 1; }

    /* arrays: 3 x max(8 x L3, 1 GiB) over the node; per socket the same total */
    long l3 = l3_bytes_total();
    long per_array = l3 * 8 > (1L << 30) ? l3 * 8 : (1L << 30);
    int reps = 6;
    double fsec = FLOP_SECONDS;
    if (quick) { per_array = 16L << 20; reps = 3; fsec = 0.05; }

    int all[MAX_CORES], n_all = 0;
    for (int i = 0; i < g_ncores; i++) all[n_all++] = g_cores[i].cpu;
    fprintf(stderr, "miniprof-calibrate: %s, %d sockets, %d physical cores used, "
                    "arrays %.2f GiB each%s\n", node.cpu_brand, g_nsockets, g_ncores,
            per_array / 1073741824.0, quick ? " (quick: NOT a calibration)" : "");

    double bw_read[64] = {0}, bw_triad[64] = {0};
    for (int s = 0; s < g_nsockets; s++) {
        int cs[MAX_CORES], n = 0;
        for (int i = 0; i < g_ncores; i++) if (g_cores[i].socket == s) cs[n++] = g_cores[i].cpu;
        if (!n) continue;
        bw_read[s]  = bandwidth(cs, n, per_array, 0, reps);
        bw_triad[s] = bandwidth(cs, n, per_array, 1, reps);
        fprintf(stderr, "  socket %d (%d cores): read %.1f GB/s, triad %.1f GB/s (DRAM view)\n",
                s, n, bw_read[s], bw_triad[s]);
    }
    double node_read  = bandwidth(all, n_all, per_array, 0, reps);
    double node_triad = bandwidth(all, n_all, per_array, 1, reps);
    fprintf(stderr, "  node: read %.1f GB/s, triad %.1f GB/s\n", node_read, node_triad);

    double fall[NFK] = {0}, fone[NFK] = {0};
    int have[NFK] = {0};
    for (int k = 0; k < NFK; k++) {
        if (!isa_ok(k_fk[k].isa)) continue;
        have[k] = 1;
        fall[k] = flops(all, n_all, k, fsec);
        fone[k] = flops(all, 1, k, fsec);
        fprintf(stderr, "  %-12s all cores %10.1f GFLOP/s   one core %7.1f\n",
                k_fk[k].key, fall[k], fone[k]);
    }

    char path[1024];
    struct stat st;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(path, sizeof(path), "%s/calibration_%s.json", out, node.hostname);
    else
        snprintf(path, sizeof(path), "%s", out);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    jw_t w;
    jw_init(&w, f);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "schema", "miniprof-calibration");
    jw_str(&w, "schema_version", "1.0.0");
    time_t tt = time(NULL);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt));
    jw_str(&w, "date_utc", date);
    jw_bool(&w, "quick", quick);
    jw_begin_obj(&w, "node");
    jw_str(&w, "hostname", node.hostname);
    jw_str(&w, "cpu_brand", node.cpu_brand);
    jw_str(&w, "uarch", node.uarch);
    jw_str(&w, "kernel", node.kernel);
    jw_int(&w, "sockets", g_nsockets);
    jw_int(&w, "cores_used", g_ncores);
    jw_int(&w, "logical_cpus", node.logical_cpus);
    jw_int(&w, "numa_nodes", node.numa_nodes);
    jw_int(&w, "l3_bytes", l3);
    jw_end_obj(&w);
    jw_begin_obj(&w, "method");
    jw_str(&w, "threads", "one per physical core in the affinity mask, pinned; SMT siblings idle");
    jw_str(&w, "bandwidth", "best of repetitions after a warm-up; per socket with threads and "
                            "first-touched memory on that socket; DRAM view: read 8 B/element, "
                            "triad 32 B/element (STREAM's 24 + write-allocate)");
    jw_int(&w, "array_bytes", per_array);
    jw_str(&w, "flops", "12 independent FMA chains in registers per thread, FMA = 2 FLOPs; "
                        "all cores at once and one core");
    jw_end_obj(&w);
    /* flat ceilings: what snapshot reads */
    jw_begin_obj(&w, "ceilings");
    jw_dbl(&w, "dram_read_gb_s_node", node_read);
    jw_dbl(&w, "dram_triad_gb_s_node", node_triad);
    jw_dbl(&w, "dram_triad_stream_gb_s_node", node_triad * 24.0 / 32.0);
    for (int s = 0; s < g_nsockets; s++) {
        char k[64];
        snprintf(k, sizeof(k), "dram_read_gb_s_socket%d", s);  jw_dbl(&w, k, bw_read[s]);
        snprintf(k, sizeof(k), "dram_triad_gb_s_socket%d", s); jw_dbl(&w, k, bw_triad[s]);
    }
    for (int k = 0; k < NFK; k++) {
        if (!have[k]) continue;
        char key[64];
        snprintf(key, sizeof(key), "gflops_%s_node", k_fk[k].key);     jw_dbl(&w, key, fall[k]);
        snprintf(key, sizeof(key), "gflops_%s_one_core", k_fk[k].key); jw_dbl(&w, key, fone[k]);
    }
    jw_end_obj(&w);
    jw_end_obj(&w);
    int rc = jw_finish(&w);
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
    return rc == 0 ? 0 : 1;
}
