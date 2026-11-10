/*
 * snapshot.c — snapshot mode: metadata, gather, versioned JSON output
 *
 * See snapshot.h for what the mode is. This file owns the lifecycle and
 * the JSON container; later phases add their metric blocks by filling a
 * per-rank struct here and emitting it inside "metrics".
 *
 * All internal communication uses PMPI_* directly, never MPI_*: once the
 * PMPI wrappers in mpi_trace.c are active in snapshot mode (phase P1),
 * miniprof's own gather traffic must not show up as application MPI time.
 *
 * Gather layout (scales with nodes, not ranks, for node metadata):
 *   - node_comm   = MPI_Comm_split_type(SHARED): ranks on one node
 *   - leader_comm = node_rank 0 of every node
 *   Node metadata goes leaders → world rank 0 over leader_comm; the small
 *   per-rank record (pid, affinity, timings) goes over MPI_COMM_WORLD.
 *   World rank 0 is always node_rank 0 of its node and rank 0 of
 *   leader_comm, because both splits are keyed by world rank.
 */

#define _GNU_SOURCE
#include "snapshot.h"
#include "sysinfo.h"
#include "json_writer.h"
#include "mpi_trace.h"
#include "io_trace.h"
#include "counters.h"
#include "ompt.h"
#include "symbolizer.h"
#include "../include/miniprof.h"

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <linux/perf_event.h>
#include <dlfcn.h>

/* ── state ───────────────────────────────────────────────────────────── */

static snapshot_level_t g_level = SNAPSHOT_BASIC;
static char   g_outfile[512]    = MINIPROF_DEFAULT_SNAPSHOT_OUTPUT;
static int    g_verbose         = 0;
static double g_init_s          = 0.0;   /* cost of snapshot_init() itself  */
static char   g_start_utc[32]   = "";
static char   g_cmdline[512]    = "";    /* rank 0 only                     */
static int    g_thread_level    = -1;
static sysinfo_node_t g_node;            /* collected at init               */
static sysinfo_rank_t g_rank;
static struct rusage  g_ru0;             /* baseline at init                */
static char           g_cnt_reason[160] = "";
static const cnt_group_t *g_cnt_group = NULL;

/* Regions reduced across ranks (rank 0 only): union of names, then
 * per-name sums / min / max. */
typedef struct {
    char     name[CNT_REGION_NAME];
    uint64_t ranks, calls, ns_sum, ns_min, ns_max;
    uint64_t ev[CNT_MAX_EVENTS];
} region_red_t;
static region_red_t g_rreg[CNT_MAX_REGIONS];
static int          g_nrreg = 0;

/* OpenMP: this rank's OMPT results; regions reduced across ranks (rank 0).
 * Regions are keyed by "<object>+0x<offset>" of the construct's code
 * address, so they match across ranks despite address randomisation. */
static omp_result_t g_omp;
#define OMP_KEY 64
typedef struct {
    char     key[OMP_KEY];
    char     name[MINIPROF_SYMBOL_LEN];   /* resolved on rank 0            */
    char     file[MINIPROF_SYMBOL_LEN];
    int      line;
    uint64_t ranks, calls, wall_ns, task_ns, wait_ns, tasks, max_team;
} omp_red_t;
static omp_red_t g_ored[OMP_MAX_REGIONS];
static int       g_nored = 0;

/* Per-rank record gathered to world rank 0. Fixed-size, sent as bytes:
 * every rank runs the same binary, so layout and padding are identical. */
typedef struct {
    int    rank;
    int    node_index;
    double elapsed_s;
    double init_s;
    double finalize_s;      /* this rank's finalize time up to the gather */
    sysinfo_rank_t info;

    /* P1: parallel runtime layer */
    double mpi_s, io_s, compute_s;
    double class_s[MPIC_COUNT];
    double cpu_user_s, cpu_sys_s, cpu_util;   /* util: fraction of allowed CPUs */
    double cpu_util_core;                     /* ... of allowed physical cores  */
    long   max_rss_kb, vcsw, ivcsw;
    uint64_t f_calls[MPIF_COUNT], f_bytes[MPIF_COUNT], f_ns[MPIF_COUNT];
    io_snap_t io;

    /* P3: hardware counters (this rank, summed over its threads) */
    int      cnt_available, cnt_n, cnt_threads, cnt_dropped;
    int      cnt_multiplexed, cnt_rdpmc;
    uint64_t cnt_reads_rdpmc, cnt_reads_syscall;
    uint64_t cnt_total[CNT_MAX_EVENTS], cnt_compute[CNT_MAX_EVENTS],
             cnt_mpi[CNT_MAX_EVENTS];

    /* P2: OpenMP (OMPT) */
    int    omp_available, omp_threads;
    double omp_parallel_s;                 /* initial thread in outermost regions */
    double omp_work_s, omp_wait_s, omp_lock_s, omp_taskwait_s;  /* sum over threads */
    double omp_work_min_s, omp_work_max_s;                      /* over threads    */
} rank_rec_t;

/* Per-node record gathered from node leaders to world rank 0. */
typedef struct {
    int            node_index;
    int            ranks_on_node;
    int            first_world_rank;
    sysinfo_node_t info;
} node_rec_t;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── level parsing ───────────────────────────────────────────────────── */

static const char *k_level_names[] = { "basic", "topdown", "roofline", "full" };

int snapshot_parse_level(const char *s)
{
    if (!s || !*s) return SNAPSHOT_BASIC;
    for (int i = 0; i < 4; i++)
        if (strcmp(s, k_level_names[i]) == 0) return i;
    return -1;
}

const char *snapshot_level_name(snapshot_level_t lvl)
{
    return (lvl >= 0 && lvl <= SNAPSHOT_FULL) ? k_level_names[lvl] : "unknown";
}

/* ── init ────────────────────────────────────────────────────────────── */

static void read_cmdline(char *dst, size_t len)
{
    dst[0] = '\0';
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    size_t n = fread(dst, 1, len - 1, f);
    fclose(f);
    dst[n] = '\0';
    /* argv entries are NUL-separated; join with spaces */
    for (size_t i = 0; i + 1 < n; i++) if (dst[i] == '\0') dst[i] = ' ';
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0')) dst[--n] = '\0';
}

void snapshot_init(int verbose)
{
    double t0 = now_s();
    g_verbose = verbose;

    int rank = 0;
    PMPI_Comm_rank(MPI_COMM_WORLD, &rank);

    const char *lvl = getenv("MINIPROF_SNAPSHOT");
    int parsed = snapshot_parse_level(lvl);
    if (parsed < 0) {
        if (rank == 0)
            fprintf(stderr, "[miniprof] unknown MINIPROF_SNAPSHOT='%s' "
                            "(expected basic|topdown|roofline|full); using basic\n", lvl);
        parsed = SNAPSHOT_BASIC;
    }
    g_level = (snapshot_level_t)parsed;

    const char *out = getenv("MINIPROF_SNAPSHOT_OUTPUT");
    if (out && *out) snprintf(g_outfile, sizeof(g_outfile), "%s", out);

    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(g_start_utc, sizeof(g_start_utc), "%Y-%m-%dT%H:%M:%SZ", &tm);

    if (PMPI_Query_thread(&g_thread_level) != MPI_SUCCESS) g_thread_level = -1;

    sysinfo_collect_node(&g_node);
    sysinfo_collect_rank(&g_rank);
    if (rank == 0) read_cmdline(g_cmdline, sizeof(g_cmdline));

    /* P3: counter group by level. topdown arrives in P4, so until then it
     * (and full) collect the base group. */
    {
        const char *grp = (g_level == SNAPSHOT_ROOFLINE) ? "vector" : "base";
        int ok = cnt_init(g_node.uarch[0] ? g_node.uarch : "generic", grp);
        if (g_verbose || (!ok && rank == 0)) {
            cnt_result_t st;
            if (!ok) cnt_collect(&st);
            fprintf(stderr, "[miniprof] rank %d: hardware counters %s%s%s\n", rank,
                    ok ? "on" : "unavailable", ok ? "" : ": ", ok ? "" : st.reason);
        }
    }

    /* P2: OpenMP statistics from here on (no-op without an OMPT runtime) */
    ompt_snapshot_begin();

    /* P1: start recording MPI and I/O, CPU-time baseline */
    mpi_trace_set_mode(MPI_TRACE_SNAPSHOT);
    io_trace_set_enabled(1);
    getrusage(RUSAGE_SELF, &g_ru0);

    g_init_s = now_s() - t0;

    if (g_verbose) {
        fprintf(stderr, "[miniprof] rank %d: SNAPSHOT mode, level=%s, output=%s, "
                        "cpu=%s (%s), cpus_allowed=%d\n",
                rank, snapshot_level_name(g_level), g_outfile,
                g_node.uarch[0] ? g_node.uarch : "unknown",
                g_node.cpu_brand, g_rank.cpus_allowed);
    }
}

/* ── JSON helpers ────────────────────────────────────────────────────── */

static void jw_str_or_null(jw_t *w, const char *key, const char *s)
{
    jw_str(w, key, (s && *s) ? s : NULL);
}

static void jw_int_or_null(jw_t *w, const char *key, int v)
{
    if (v < 0) jw_null(w, key);
    else       jw_int(w, key, v);
}

static const char *thread_level_name(int lvl)
{
    if (lvl == MPI_THREAD_SINGLE)     return "single";
    if (lvl == MPI_THREAD_FUNNELED)   return "funneled";
    if (lvl == MPI_THREAD_SERIALIZED) return "serialized";
    if (lvl == MPI_THREAD_MULTIPLE)   return "multiple";
    return NULL;
}

static void write_node(jw_t *w, const node_rec_t *nr)
{
    const sysinfo_node_t *n = &nr->info;
    jw_begin_obj(w, NULL);
    jw_int(w, "index", nr->node_index);
    jw_str_or_null(w, "hostname", n->hostname);
    jw_int(w, "ranks", nr->ranks_on_node);
    jw_int(w, "first_rank", nr->first_world_rank);

    jw_begin_obj(w, "cpu");
    jw_str_or_null(w, "vendor", n->cpu_vendor);
    jw_int_or_null(w, "family", n->cpu_family);
    jw_int_or_null(w, "model", n->cpu_model);
    jw_int_or_null(w, "stepping", n->cpu_stepping);
    jw_str_or_null(w, "brand", n->cpu_brand);
    jw_str_or_null(w, "uarch", n->uarch);
    jw_str_or_null(w, "uarch_name", n->uarch_name);
    jw_end_obj(w);

    jw_int_or_null(w, "logical_cpus", n->logical_cpus);
    jw_int_or_null(w, "numa_nodes", n->numa_nodes);
    if (n->smt_active < 0) jw_null(w, "smt_active");
    else                   jw_bool(w, "smt_active", n->smt_active);
    jw_str_or_null(w, "kernel", n->kernel);
    if (n->perf_event_paranoid == SYSINFO_PARANOID_UNKNOWN) jw_null(w, "perf_event_paranoid");
    else jw_int(w, "perf_event_paranoid", n->perf_event_paranoid);

    /* Exclusivity heuristic. procs_running at init counts this job's own
     * runnable ranks too, so anything well beyond ranks_on_node suggests
     * another workload shares the node — which matters for node-level
     * counters (memory controllers, data fabric) in later phases. It is a
     * hint, not proof: a node can be busy with daemons, or another job
     * can be idle at the instant we look. */
    jw_begin_obj(w, "exclusivity");
    if (n->loadavg1 < 0) jw_null(w, "loadavg1_at_init");
    else                 jw_dbl(w, "loadavg1_at_init", n->loadavg1);
    jw_int_or_null(w, "procs_running_at_init", n->procs_running);
    if (n->procs_running < 0) {
        jw_null(w, "shared_suspected");
    } else {
        int others = n->procs_running - nr->ranks_on_node;
        jw_bool(w, "shared_suspected", others > 2);
    }
    jw_end_obj(w);

    jw_end_obj(w);
}

static void write_rank(jw_t *w, const rank_rec_t *rr)
{
    jw_begin_obj(w, NULL);
    jw_int(w, "rank", rr->rank);
    jw_int(w, "node", rr->node_index);
    jw_int(w, "pid", rr->info.pid);
    jw_int_or_null(w, "cpus_allowed", rr->info.cpus_allowed);
    jw_int_or_null(w, "cores_allowed", rr->info.cores_allowed);
    jw_str_or_null(w, "cpus_allowed_list", rr->info.cpus_allowed_list);
    jw_dbl(w, "elapsed_s", rr->elapsed_s);
    jw_dbl(w, "compute_s", rr->compute_s);
    jw_dbl(w, "mpi_s", rr->mpi_s);
    jw_dbl(w, "io_s", rr->io_s);
    jw_dbl(w, "cpu_util_pct", 100.0 * rr->cpu_util);
    jw_dbl(w, "cpu_util_per_core_pct", 100.0 * rr->cpu_util_core);
    if (rr->omp_available) {
        jw_int(w, "omp_threads", rr->omp_threads);
        jw_dbl(w, "omp_parallel_s", rr->omp_parallel_s);
    }
    jw_end_obj(w);
}

/* ── P1: parallel runtime block ──────────────────────────────────────── */

typedef struct { double min, avg, max, sum; } stat_t;

/* Accessor-based reduction over the gathered rank records. `arg` selects
 * an element for array-valued fields (function or class index). */
typedef double (*rank_get_t)(const rank_rec_t *, int arg);

static stat_t stats(const rank_rec_t *r, int n, rank_get_t get, int arg)
{
    stat_t s = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        double v = get(&r[i], arg);
        if (i == 0 || v < s.min) s.min = v;
        if (i == 0 || v > s.max) s.max = v;
        s.sum += v;
    }
    s.avg = n > 0 ? s.sum / n : 0.0;
    return s;
}

static void jw_stat(jw_t *w, const char *key, stat_t s)
{
    jw_begin_obj(w, key);
    jw_dbl(w, "min", s.min);
    jw_dbl(w, "avg", s.avg);
    jw_dbl(w, "max", s.max);
    jw_end_obj(w);
}

/* (max - avg) / max, in percent; 0 when max is 0 */
static double imbalance_pct(stat_t s) { return s.max > 0 ? 100.0 * (s.max - s.avg) / s.max : 0.0; }
static double pct(double part, double whole) { return whole > 0 ? 100.0 * part / whole : 0.0; }

static double g_elapsed (const rank_rec_t *r, int a) { (void)a; return r->elapsed_s; }
static double g_compute (const rank_rec_t *r, int a) { (void)a; return r->compute_s; }
static double g_mpi     (const rank_rec_t *r, int a) { (void)a; return r->mpi_s; }
static double g_io      (const rank_rec_t *r, int a) { (void)a; return r->io_s; }
static double g_class   (const rank_rec_t *r, int c) { return r->class_s[c]; }
static double g_func_s  (const rank_rec_t *r, int f) { return r->f_ns[f] * 1e-9; }
static double g_cpu_util(const rank_rec_t *r, int a) { (void)a; return 100.0 * r->cpu_util; }
static double g_cpu_util_core(const rank_rec_t *r, int a) { (void)a; return 100.0 * r->cpu_util_core; }
static double g_cpu_user(const rank_rec_t *r, int a) { (void)a; return r->cpu_user_s; }
static double g_cpu_sys (const rank_rec_t *r, int a) { (void)a; return r->cpu_sys_s; }
static double g_rss_mb  (const rank_rec_t *r, int a) { (void)a; return r->max_rss_kb / 1024.0; }

/* I/O accessors: arg = 0 posix, 1 mpiio */
static const io_counters_t *ioc(const rank_rec_t *r, int k) { return k ? &r->io.mpiio : &r->io.posix; }
static double g_io_read_s (const rank_rec_t *r, int k) { return ioc(r, k)->read_ns  * 1e-9; }
static double g_io_write_s(const rank_rec_t *r, int k) { return ioc(r, k)->write_ns * 1e-9; }
static double g_io_meta_s (const rank_rec_t *r, int k) { return ioc(r, k)->meta_ns  * 1e-9; }

static void write_io_kind(jw_t *w, const char *key, const rank_rec_t *r, int n, int k)
{
    uint64_t oc = 0, cc = 0, rc = 0, wc = 0, sc = 0, rb = 0, wb = 0;
    for (int i = 0; i < n; i++) {
        const io_counters_t *c = ioc(&r[i], k);
        oc += c->open_calls;  cc += c->close_calls; rc += c->read_calls;
        wc += c->write_calls; sc += c->sync_calls;
        rb += c->read_bytes;  wb += c->write_bytes;
    }
    jw_begin_obj(w, key);
    jw_begin_obj(w, "calls");
    jw_int(w, "open", (int64_t)oc);  jw_int(w, "close", (int64_t)cc);
    jw_int(w, "read", (int64_t)rc);  jw_int(w, "write", (int64_t)wc);
    jw_int(w, "sync", (int64_t)sc);
    jw_end_obj(w);
    jw_begin_obj(w, "bytes");
    jw_int(w, "read", (int64_t)rb);
    jw_int(w, "write", (int64_t)wb);
    jw_end_obj(w);
    jw_begin_obj(w, "time_s");
    jw_stat(w, "read",  stats(r, n, g_io_read_s,  k));
    jw_stat(w, "write", stats(r, n, g_io_write_s, k));
    jw_stat(w, "meta",  stats(r, n, g_io_meta_s,  k));
    jw_end_obj(w);
    jw_end_obj(w);
}

static const rank_rec_t *g_sort_ranks;   /* qsort context (single-threaded) */
static int cmp_rank_compute_desc(const void *a, const void *b)
{
    double x = g_sort_ranks[*(const int *)a].compute_s;
    double y = g_sort_ranks[*(const int *)b].compute_s;
    return (x < y) - (x > y);
}

typedef struct { int id; uint64_t ns; } func_order_t;
static int cmp_func_desc(const void *a, const void *b)
{
    uint64_t x = ((const func_order_t *)a)->ns, y = ((const func_order_t *)b)->ns;
    return (x < y) - (x > y);
}

static void write_parallel(jw_t *w, const rank_rec_t *r, int n, const mpi_snap_t *hist)
{
    stat_t el   = stats(r, n, g_elapsed, 0);
    stat_t comp = stats(r, n, g_compute, 0);
    stat_t mpi  = stats(r, n, g_mpi, 0);
    stat_t io   = stats(r, n, g_io, 0);

    jw_begin_obj(w, "parallel");

    jw_begin_obj(w, "definitions");
    jw_str(w, "compute_s", "elapsed - MPI time - I/O time, per rank");
    jw_str(w, "imbalance_pct", "(max - avg) / max over ranks");
    jw_str(w, "imbalance_cost_pct",
           "(max - avg) compute time as a share of elapsed: what perfect balance would save");
    jw_str(w, "bytes", "this rank's share of each call's data; receives count the posted size");
    jw_str(w, "message_sizes", "bin 0 = 0 bytes; bin b = [2^(b-1), 2^b) bytes");
    jw_end_obj(w);

    jw_begin_obj(w, "time_split_pct");
    jw_dbl(w, "compute", pct(comp.avg, el.avg));
    jw_dbl(w, "mpi",     pct(mpi.avg,  el.avg));
    jw_dbl(w, "io",      pct(io.avg,   el.avg));
    jw_end_obj(w);

    /* compute */
    jw_begin_obj(w, "compute");
    jw_stat(w, "time_s", comp);
    jw_dbl(w, "imbalance_pct", imbalance_pct(comp));
    jw_dbl(w, "imbalance_cost_pct", pct(comp.max - comp.avg, el.max));
    {
        int *idx = malloc((size_t)n * sizeof(int));
        jw_begin_arr(w, "slowest_ranks");
        if (idx) {
            for (int i = 0; i < n; i++) idx[i] = i;
            g_sort_ranks = r;
            qsort(idx, (size_t)n, sizeof(int), cmp_rank_compute_desc);
            for (int i = 0; i < n && i < 5; i++) {
                jw_begin_obj(w, NULL);
                jw_int(w, "rank", r[idx[i]].rank);
                jw_dbl(w, "compute_s", r[idx[i]].compute_s);
                jw_end_obj(w);
            }
            free(idx);
        }
        jw_end_arr(w);
    }
    jw_end_obj(w);

    /* MPI */
    jw_begin_obj(w, "mpi");
    jw_stat(w, "time_s", mpi);
    jw_dbl(w, "pct_of_elapsed", pct(mpi.avg, el.avg));
    jw_dbl(w, "imbalance_pct", imbalance_pct(mpi));

    jw_begin_obj(w, "by_class");
    for (int c = 0; c < MPIC_COUNT; c++) {
        uint64_t calls = 0, bytes = 0;
        for (int i = 0; i < n; i++)
            for (int f = 0; f < MPIF_COUNT; f++)
                if (mpi_trace_func_class(f) == c) { calls += r[i].f_calls[f]; bytes += r[i].f_bytes[f]; }
        stat_t cs = stats(r, n, g_class, c);
        jw_begin_obj(w, mpi_trace_class_name(c));
        jw_stat(w, "time_s", cs);
        jw_dbl(w, "pct_of_elapsed", pct(cs.avg, el.avg));
        jw_int(w, "calls", (int64_t)calls);
        jw_int(w, "bytes", (int64_t)bytes);
        jw_end_obj(w);
    }
    jw_end_obj(w);

    /* functions, most expensive (summed over ranks) first */
    func_order_t order[MPIF_COUNT];
    int nf = 0;
    for (int f = 0; f < MPIF_COUNT; f++) {
        uint64_t calls = 0, ns = 0;
        for (int i = 0; i < n; i++) { calls += r[i].f_calls[f]; ns += r[i].f_ns[f]; }
        if (calls) { order[nf].id = f; order[nf].ns = ns; nf++; }
    }
    qsort(order, (size_t)nf, sizeof(order[0]), cmp_func_desc);
    jw_begin_arr(w, "by_function");
    for (int k = 0; k < nf; k++) {
        int f = order[k].id;
        uint64_t calls = 0, bytes = 0;
        for (int i = 0; i < n; i++) { calls += r[i].f_calls[f]; bytes += r[i].f_bytes[f]; }
        stat_t fs = stats(r, n, g_func_s, f);
        jw_begin_obj(w, NULL);
        jw_str(w, "name", mpi_trace_func_name(f));
        jw_str(w, "class", mpi_trace_class_name(mpi_trace_func_class(f)));
        jw_int(w, "calls", (int64_t)calls);
        jw_int(w, "bytes", (int64_t)bytes);
        jw_dbl(w, "avg_bytes_per_call", calls ? (double)bytes / calls : 0.0);
        jw_stat(w, "time_s", fs);
        jw_dbl(w, "pct_of_elapsed", pct(fs.avg, el.avg));
        jw_dbl(w, "imbalance_pct", imbalance_pct(fs));
        jw_end_obj(w);
    }
    jw_end_arr(w);

    /* message-size histogram, trimmed after the last non-empty bin */
    int last = 0;
    for (int c = 0; c < MPIC_COUNT; c++)
        for (int b = 0; b < MPI_HIST_BINS; b++)
            if (hist->hist_calls[c][b] && b > last) last = b;
    jw_begin_obj(w, "message_sizes");
    jw_begin_arr(w, "bin_lower_bytes");
    for (int b = 0; b <= last; b++) jw_int(w, NULL, b == 0 ? 0 : (int64_t)1 << (b - 1));
    jw_end_arr(w);
    for (int c = 0; c < MPIC_COUNT; c++) {
        uint64_t any = 0;
        for (int b = 0; b <= last; b++) any |= hist->hist_calls[c][b];
        if (!any) continue;
        jw_begin_obj(w, mpi_trace_class_name(c));
        jw_begin_arr(w, "calls");
        for (int b = 0; b <= last; b++) jw_int(w, NULL, (int64_t)hist->hist_calls[c][b]);
        jw_end_arr(w);
        jw_begin_arr(w, "time_s");
        for (int b = 0; b <= last; b++) jw_dbl(w, NULL, hist->hist_ns[c][b] * 1e-9);
        jw_end_arr(w);
        jw_end_obj(w);
    }
    jw_end_obj(w);
    jw_end_obj(w);   /* mpi */

    /* I/O */
    jw_begin_obj(w, "io");
    jw_stat(w, "time_s", io);
    jw_dbl(w, "pct_of_elapsed", pct(io.avg, el.avg));
    write_io_kind(w, "posix", r, n, 0);
    write_io_kind(w, "mpiio", r, n, 1);
    jw_end_obj(w);

    /* CPU */
    long vcsw = 0, ivcsw = 0;
    for (int i = 0; i < n; i++) { vcsw += r[i].vcsw; ivcsw += r[i].ivcsw; }
    jw_begin_obj(w, "cpu");
    jw_stat(w, "utilisation_pct", stats(r, n, g_cpu_util, 0));
    jw_stat(w, "utilisation_per_core_pct", stats(r, n, g_cpu_util_core, 0));
    jw_stat(w, "user_s", stats(r, n, g_cpu_user, 0));
    jw_stat(w, "sys_s",  stats(r, n, g_cpu_sys, 0));
    jw_stat(w, "max_rss_mb", stats(r, n, g_rss_mb, 0));
    jw_int(w, "voluntary_ctx_switches", vcsw);
    jw_int(w, "involuntary_ctx_switches", ivcsw);
    jw_str(w, "note", "utilisation = CPU time / (elapsed x hardware threads the rank may run "
                      "on); per_core uses physical cores instead, which is the meaningful one "
                      "when a rank is bound to a whole SMT core. MPI libraries usually "
                      "busy-poll while waiting, so MPI time counts as busy");
    jw_end_obj(w);

    jw_end_obj(w);   /* parallel */
}

/* ── P3: counters block ─────────────────────────────────────────────── */

static int kind_index(cnt_kind_t k, int n_active)
{
    if (!g_cnt_group) return -1;
    for (int i = 0; i < n_active; i++) if (g_cnt_group->ev[i].kind == k) return i;
    return -1;
}

static double ratio(double a, double b) { return b > 0 ? a / b : 0.0; }

static void write_region_list(jw_t *w, int n_active, double el_max)
{
    int iC = kind_index(CK_CYCLES, n_active), iI = kind_index(CK_INSTRUCTIONS, n_active);
    int iF = kind_index(CK_FLOPS, n_active);
    jw_begin_arr(w, "regions");
    for (int u = 0; u < g_nrreg; u++) {
        const region_red_t *r = &g_rreg[u];
        double avg_s = r->ranks ? r->ns_sum * 1e-9 / r->ranks : 0.0;
        jw_begin_obj(w, NULL);
        jw_str(w, "name", r->name);
        jw_int(w, "ranks", (int64_t)r->ranks);
        jw_int(w, "calls", (int64_t)r->calls);
        jw_begin_obj(w, "time_s");
        jw_dbl(w, "min", r->ns_min * 1e-9);
        jw_dbl(w, "avg", avg_s);
        jw_dbl(w, "max", r->ns_max * 1e-9);
        jw_end_obj(w);
        jw_dbl(w, "pct_of_elapsed", el_max > 0 ? 100.0 * avg_s / el_max : 0.0);
        if (n_active > 0) {
            jw_begin_obj(w, "totals");
            for (int i = 0; i < n_active; i++)
                jw_int(w, cnt_kind_name(g_cnt_group->ev[i].kind), (int64_t)r->ev[i]);
            jw_end_obj(w);
            jw_begin_obj(w, "derived");
            if (iC >= 0 && iI >= 0) jw_dbl(w, "ipc", ratio((double)r->ev[iI], (double)r->ev[iC]));
            if (iF >= 0) {
                jw_dbl(w, "gflops_per_s_per_rank",
                       r->ranks && avg_s > 0 ? r->ev[iF] / (double)r->ranks / avg_s * 1e-9 : 0.0);
                if (iC >= 0) jw_dbl(w, "flops_per_cycle", ratio((double)r->ev[iF], (double)r->ev[iC]));
            }
            jw_end_obj(w);
        }
        jw_end_obj(w);
    }
    jw_end_arr(w);
}

static void write_counters(jw_t *w, const rank_rec_t *r, int n)
{
    int avail = 0, n_active = CNT_MAX_EVENTS, mux = 0;
    long threads = 0, dropped = 0;
    uint64_t rd_fast = 0, rd_sys = 0;
    double el_max = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].elapsed_s > el_max) el_max = r[i].elapsed_s;
        if (!r[i].cnt_available) continue;
        avail++;
        if (r[i].cnt_n < n_active) n_active = r[i].cnt_n;
        rd_fast += r[i].cnt_reads_rdpmc;
        rd_sys  += r[i].cnt_reads_syscall;
        mux += r[i].cnt_multiplexed;
        threads += r[i].cnt_threads;
        dropped += r[i].cnt_dropped;
    }
    if (!avail) n_active = 0;

    jw_begin_obj(w, "counters");
    jw_bool(w, "available", avail > 0);
    if (!avail) {
        jw_str(w, "reason", g_cnt_reason[0] ? g_cnt_reason : "unknown");
        write_region_list(w, 0, el_max);
        jw_end_obj(w);
        return;
    }
    jw_int(w, "ranks_without_counters", n - avail);
    jw_str(w, "uarch", g_cnt_group->uarch);
    jw_str(w, "group", g_cnt_group->group);
    /* boundary/region reads: rdpmc (user space) vs read() syscall. Mostly
     * syscall means rdpmc is disabled (sysfs rdpmc=0, MINIPROF_NO_RDPMC)
     * or events were often off the PMU — expect higher per-call cost. */
    jw_str(w, "read_method", rd_fast + rd_sys == 0 ? "none" :
                             rd_sys == 0 ? "rdpmc" : rd_fast == 0 ? "syscall" : "mixed");
    jw_begin_obj(w, "reads");
    jw_int(w, "rdpmc", (int64_t)rd_fast);
    jw_int(w, "syscall", (int64_t)rd_sys);
    jw_end_obj(w);
    jw_bool(w, "user_space_only", 1);
    jw_bool(w, "compute_mpi_split", cnt_split_enabled());
    jw_int(w, "threads_counted", threads);
    jw_int(w, "threads_dropped", dropped);
    jw_int(w, "multiplexed_ranks", mux);

    jw_begin_arr(w, "events");
    for (int i = 0; i < g_cnt_group->n_events; i++) {
        const cnt_event_t *e = &g_cnt_group->ev[i];
        char cfg[24];
        snprintf(cfg, sizeof(cfg), "0x%llx", (unsigned long long)e->config);
        jw_begin_obj(w, NULL);
        jw_str(w, "key", cnt_kind_name(e->kind));
        jw_str(w, "perf_name", e->perf_name);
        jw_str(w, "type", e->type == PERF_TYPE_RAW ? "raw" :
                          e->type == PERF_TYPE_SOFTWARE ? "software" : "hardware");
        jw_str(w, "config", cfg);
        jw_bool(w, "optional", i >= g_cnt_group->n_required);
        jw_bool(w, "active", i < n_active);
        jw_end_obj(w);
    }
    jw_end_arr(w);

    jw_begin_obj(w, "definitions");
    jw_str(w, "compute", "counts outside MPI calls (includes I/O); mpi = inside MPI calls");
    jw_str(w, "dram_fills", "L1D fills sourced from DRAM/IO, x64 B per line; misses lines the "
                            "L2 prefetcher brought in and all writebacks, so it is a lower "
                            "bound on DRAM traffic, not a bandwidth measurement");
    jw_str(w, "ghz_effective", "cycles / user CPU time (user-space counts)");
    jw_str(w, "fp_ops_width", "FP-unit ops by vector width, including FP loads, stores and "
                              "register moves: shows how much vector hardware is used, not "
                              "the share of arithmetic that is vectorised");
    jw_end_obj(w);

    uint64_t tot[CNT_MAX_EVENTS] = {0}, comp[CNT_MAX_EVENTS] = {0}, mpi[CNT_MAX_EVENTS] = {0};
    for (int i = 0; i < n; i++) {
        if (!r[i].cnt_available) continue;
        for (int e = 0; e < n_active; e++) {
            tot[e] += r[i].cnt_total[e]; comp[e] += r[i].cnt_compute[e]; mpi[e] += r[i].cnt_mpi[e];
        }
    }
    const char *bucket_names[3] = { "totals", "compute", "mpi" };
    uint64_t *buckets[3] = { tot, comp, mpi };
    for (int b = 0; b < 3; b++) {
        jw_begin_obj(w, bucket_names[b]);
        for (int e = 0; e < n_active; e++)
            jw_int(w, cnt_kind_name(g_cnt_group->ev[e].kind), (int64_t)buckets[b][e]);
        jw_end_obj(w);
    }

    /* per-rank min/avg/max over ranks that have counters */
    jw_begin_obj(w, "per_rank");
    for (int e = 0; e < n_active; e++) {
        stat_t st = { 0, 0, 0, 0 };
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (!r[i].cnt_available) continue;
            double v = (double)r[i].cnt_total[e];
            if (k == 0 || v < st.min) st.min = v;
            if (k == 0 || v > st.max) st.max = v;
            st.sum += v; k++;
        }
        st.avg = k ? st.sum / k : 0;
        jw_stat(w, cnt_kind_name(g_cnt_group->ev[e].kind), st);
    }
    jw_end_obj(w);

    int iC = kind_index(CK_CYCLES, n_active), iI = kind_index(CK_INSTRUCTIONS, n_active);
    int iF = kind_index(CK_FLOPS, n_active), iL2 = kind_index(CK_L2_MISSES, n_active);
    int iD = kind_index(CK_DRAM_FILLS, n_active), iR = kind_index(CK_DRAM_FILLS_REMOTE, n_active);
    int iS = kind_index(CK_FP_SCALAR, n_active), i128 = kind_index(CK_FP_128, n_active);
    int i256 = kind_index(CK_FP_256, n_active), i512 = kind_index(CK_FP_512, n_active);

    jw_begin_obj(w, "derived");
    if (iC >= 0 && iI >= 0) {
        jw_dbl(w, "ipc", ratio((double)tot[iI], (double)tot[iC]));
        jw_dbl(w, "ipc_compute", ratio((double)comp[iI], (double)comp[iC]));
        jw_dbl(w, "ipc_mpi", ratio((double)mpi[iI], (double)mpi[iC]));
        stat_t st = { 0, 0, 0, 0 };
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (!r[i].cnt_available) continue;
            double v = ratio((double)r[i].cnt_total[iI], (double)r[i].cnt_total[iC]);
            if (k == 0 || v < st.min) st.min = v;
            if (k == 0 || v > st.max) st.max = v;
            st.sum += v; k++;
        }
        st.avg = k ? st.sum / k : 0;
        jw_stat(w, "ipc_per_rank", st);
    }
    if (iC >= 0) {
        double cyc = 0, ut = 0;
        for (int i = 0; i < n; i++)
            if (r[i].cnt_available) { cyc += (double)r[i].cnt_total[iC]; ut += r[i].cpu_user_s; }
        jw_dbl(w, "ghz_effective", ut > 0 ? cyc / ut * 1e-9 : 0.0);
    }
    if (iF >= 0) {
        jw_dbl(w, "gflops_per_s", el_max > 0 ? tot[iF] / el_max * 1e-9 : 0.0);
        double comp_s = 0;
        for (int i = 0; i < n; i++) if (r[i].cnt_available) comp_s += r[i].compute_s;
        /* rate while computing: excludes time waiting in MPI/I/O */
        jw_dbl(w, "gflops_per_s_per_rank_in_compute", comp_s > 0 ? comp[iF] / comp_s * 1e-9 : 0.0);
        if (iC >= 0) jw_dbl(w, "flops_per_cycle", ratio((double)tot[iF], (double)tot[iC]));
        jw_dbl(w, "flops_in_mpi_pct", 100.0 * ratio((double)mpi[iF], (double)tot[iF]));
    }
    if (iL2 >= 0 && iI >= 0)
        jw_dbl(w, "l2_misses_per_kilo_instr", 1000.0 * ratio((double)tot[iL2], (double)tot[iI]));
    if (iD >= 0) {
        double bytes = 64.0 * (double)tot[iD];
        jw_dbl(w, "dram_fill_bytes", bytes);
        jw_dbl(w, "dram_fill_gb_per_s", el_max > 0 ? bytes / el_max * 1e-9 : 0.0);
        if (iR >= 0) jw_dbl(w, "remote_dram_fill_pct", 100.0 * ratio((double)tot[iR], (double)tot[iD]));
        if (iF >= 0) jw_dbl(w, "flops_per_dram_fill_byte", ratio((double)tot[iF], bytes));
    }
    if (iS >= 0 && i128 >= 0 && i256 >= 0 && i512 >= 0) {
        double sc = (double)tot[iS], p = (double)tot[i128] + (double)tot[i256] + (double)tot[i512];
        jw_dbl(w, "packed_fp_ops_pct", 100.0 * ratio(p, sc + p));
        jw_begin_obj(w, "fp_ops_width_pct");
        jw_dbl(w, "scalar", 100.0 * ratio(sc, sc + p));
        jw_dbl(w, "packed128", 100.0 * ratio((double)tot[i128], sc + p));
        jw_dbl(w, "packed256", 100.0 * ratio((double)tot[i256], sc + p));
        jw_dbl(w, "packed512", 100.0 * ratio((double)tot[i512], sc + p));
        jw_end_obj(w);
    }
    jw_end_obj(w);

    write_region_list(w, n_active, el_max);
    jw_end_obj(w);
}

/* ── P2: OpenMP block ───────────────────────────────────────────────── */

static double g_omp_par (const rank_rec_t *r, int a) { (void)a; return r->omp_parallel_s; }
static double g_omp_ser (const rank_rec_t *r, int a) { (void)a; double s = r->elapsed_s - r->omp_parallel_s; return s > 0 ? s : 0; }
static double g_omp_thr_imb(const rank_rec_t *r, int a)
{
    (void)a;
    if (r->omp_threads <= 0 || r->omp_work_max_s <= 0) return 0;
    double avg = r->omp_work_s / r->omp_threads;
    return 100.0 * (r->omp_work_max_s - avg) / r->omp_work_max_s;
}

static void write_openmp(jw_t *w, const rank_rec_t *r, int n)
{
    int avail = 0;
    for (int i = 0; i < n; i++) avail += r[i].omp_available;
    jw_begin_obj(w, "openmp");
    jw_bool(w, "available", avail > 0);
    if (!avail) {
        jw_str(w, "reason", g_omp.reason[0] ? g_omp.reason : "unknown");
        jw_end_obj(w);
        return;
    }
    jw_int(w, "ranks_without_ompt", n - avail);
    jw_str(w, "runtime", g_omp.runtime);

    jw_begin_obj(w, "definitions");
    jw_str(w, "parallel_s", "time the initial thread spent inside outermost parallel regions");
    jw_str(w, "serial_s", "elapsed - parallel_s: time outside any parallel region");
    jw_str(w, "barrier_wait", "time team members waited at barriers (explicit, implicit at the end "
                              "of worksharing constructs and parallel regions)");
    jw_str(w, "imbalance_pct", "barrier wait / team time: share of thread time spent waiting "
                               "for slower team members");
    jw_str(w, "thread_imbalance_pct", "per rank, (max - avg) / max of per-thread work time");
    jw_end_obj(w);

    double el = 0, par = 0, work = 0, wait = 0, lock = 0, tw = 0;
    long threads = 0;
    for (int i = 0; i < n; i++) {
        if (!r[i].omp_available) continue;
        el += r[i].elapsed_s; par += r[i].omp_parallel_s;
        work += r[i].omp_work_s; wait += r[i].omp_wait_s;
        lock += r[i].omp_lock_s; tw += r[i].omp_taskwait_s;
        threads += r[i].omp_threads;
    }
    jw_int(w, "threads", threads);
    jw_stat(w, "parallel_s", stats(r, n, g_omp_par, 0));
    jw_stat(w, "serial_s", stats(r, n, g_omp_ser, 0));
    jw_dbl(w, "parallel_pct_of_elapsed", pct(par, el));
    jw_dbl(w, "serial_pct_of_elapsed", pct(el - par, el));
    jw_begin_obj(w, "thread_time_s");
    jw_dbl(w, "work", work);
    jw_dbl(w, "barrier_wait", wait);
    jw_dbl(w, "lock_wait", lock);
    jw_dbl(w, "task_wait", tw);
    jw_end_obj(w);
    jw_dbl(w, "imbalance_pct", pct(wait, work + wait));
    jw_dbl(w, "lock_wait_pct", pct(lock, work + wait));
    jw_stat(w, "thread_imbalance_pct", stats(r, n, g_omp_thr_imb, 0));

    jw_begin_arr(w, "regions");
    for (int u = 0; u < g_nored; u++) {
        const omp_red_t *o = &g_ored[u];
        jw_begin_obj(w, NULL);
        jw_str(w, "name", o->name[0] ? o->name : o->key);
        if (o->file[0] && o->line > 0) {
            char loc[MINIPROF_SYMBOL_LEN + 16];
            const char *b = strrchr(o->file, '/');
            snprintf(loc, sizeof(loc), "%s:%d", b ? b + 1 : o->file, o->line);
            jw_str(w, "location", loc);
        }
        jw_str(w, "key", o->key);
        jw_int(w, "ranks", (int64_t)o->ranks);
        jw_int(w, "calls", (int64_t)o->calls);
        jw_dbl(w, "wall_s_per_rank", o->ranks ? o->wall_ns * 1e-9 / o->ranks : 0.0);
        jw_dbl(w, "avg_team", o->calls ? (double)o->tasks / o->calls : 0.0);
        jw_int(w, "max_team", (int64_t)o->max_team);
        jw_dbl(w, "barrier_wait_s", o->wait_ns * 1e-9);
        jw_dbl(w, "imbalance_pct", o->task_ns ? 100.0 * (double)o->wait_ns / (double)o->task_ns : 0.0);
        jw_end_obj(w);
    }
    jw_end_arr(w);
    jw_end_obj(w);
}

static int write_json(const rank_rec_t *ranks, int nranks,
                      const node_rec_t *nodes, int nnodes,
                      const char *mpi_lib, double finalize_s,
                      const mpi_snap_t *hist)
{
    FILE *f = fopen(g_outfile, "w");
    if (!f) {
        fprintf(stderr, "[miniprof] cannot open snapshot output '%s'\n", g_outfile);
        return -1;
    }

    double el_min = 1e300, el_max = 0.0, el_sum = 0.0, init_max = 0.0;
    int rpn_min = 1 << 30, rpn_max = 0;
    int cpus_min = 1 << 30, cpus_max = -1;
    for (int i = 0; i < nranks; i++) {
        double e = ranks[i].elapsed_s;
        if (e < el_min) el_min = e;
        if (e > el_max) el_max = e;
        el_sum += e;
        if (ranks[i].init_s > init_max) init_max = ranks[i].init_s;
        int c = ranks[i].info.cpus_allowed;
        if (c >= 0 && c < cpus_min) cpus_min = c;
        if (c > cpus_max) cpus_max = c;
    }
    for (int i = 0; i < nnodes; i++) {
        if (nodes[i].ranks_on_node < rpn_min) rpn_min = nodes[i].ranks_on_node;
        if (nodes[i].ranks_on_node > rpn_max) rpn_max = nodes[i].ranks_on_node;
    }

    jw_t w;
    jw_init(&w, f);
    jw_begin_obj(&w, NULL);

    jw_str(&w, "schema", MINIPROF_SNAPSHOT_SCHEMA);
    jw_str(&w, "schema_version", MINIPROF_SNAPSHOT_SCHEMA_VERSION);

    jw_begin_obj(&w, "tool");
    jw_str(&w, "name", "miniprof");
    jw_str(&w, "version", MINIPROF_VERSION);
    jw_end_obj(&w);

    jw_begin_obj(&w, "run");
    jw_str_or_null(&w, "start_utc", g_start_utc);
    jw_str_or_null(&w, "command", g_cmdline);
    jw_str(&w, "level", snapshot_level_name(g_level));
    jw_int(&w, "nranks", nranks);
    jw_int(&w, "nnodes", nnodes);
    jw_begin_obj(&w, "ranks_per_node");
    jw_int(&w, "min", rpn_min);
    jw_int(&w, "max", rpn_max);
    jw_end_obj(&w);
    jw_begin_obj(&w, "cpus_per_rank");
    if (cpus_max < 0) { jw_null(&w, "min"); jw_null(&w, "max"); }
    else { jw_int(&w, "min", cpus_min); jw_int(&w, "max", cpus_max); }
    jw_end_obj(&w);
    jw_str_or_null(&w, "omp_num_threads", getenv("OMP_NUM_THREADS"));
    jw_str_or_null(&w, "mpi_thread_level", thread_level_name(g_thread_level));
    jw_str_or_null(&w, "mpi_library", mpi_lib);
    jw_str_or_null(&w, "slurm_job_id", getenv("SLURM_JOB_ID"));
    jw_begin_obj(&w, "elapsed_s");
    jw_dbl(&w, "min", el_min);
    jw_dbl(&w, "avg", nranks > 0 ? el_sum / nranks : 0.0);
    jw_dbl(&w, "max", el_max);
    jw_end_obj(&w);
    jw_end_obj(&w);

    /* Profiler's own cost, measured from inside: init (per rank, max) +
     * rank 0's finalize work before this file was opened. Wall-clock
     * comparison against an uninstrumented run is `make overhead-3d`. */
    jw_begin_obj(&w, "overhead");
    jw_dbl(&w, "init_s_max", init_max);
    jw_dbl(&w, "finalize_s", finalize_s);
    jw_dbl(&w, "fraction_of_elapsed",
           el_max > 0 ? (init_max + finalize_s) / el_max : 0.0);
    jw_end_obj(&w);

    jw_begin_arr(&w, "nodes");
    for (int i = 0; i < nnodes; i++) write_node(&w, &nodes[i]);
    jw_end_arr(&w);

    jw_begin_arr(&w, "ranks");
    for (int i = 0; i < nranks; i++) write_rank(&w, &ranks[i]);
    jw_end_arr(&w);

    /* Metric blocks, filled phase by phase (see the implementation plan):
     * parallel=P1, openmp=P2, counters=P3, topdown=P4, memory=P5,
     * roofline=P6. `collected` lists which ones this run actually has, so
     * consumers never have to infer it from nulls. */
    jw_begin_obj(&w, "metrics");
    jw_begin_arr(&w, "collected");
    jw_str(&w, NULL, "parallel");
    jw_str(&w, NULL, "counters");
    jw_str(&w, NULL, "openmp");
    jw_end_arr(&w);
    write_parallel(&w, ranks, nranks, hist);
    write_counters(&w, ranks, nranks);
    write_openmp(&w, ranks, nranks);
    jw_null(&w, "topdown");
    jw_null(&w, "memory");
    jw_null(&w, "roofline");
    jw_end_obj(&w);

    jw_end_obj(&w);
    int rc = jw_finish(&w);
    if (fclose(f) != 0) rc = -1;
    if (rc != 0)
        fprintf(stderr, "[miniprof] error writing snapshot JSON '%s'\n", g_outfile);
    return rc;
}

/* ── finalize ────────────────────────────────────────────────────────── */

/* Fill this rank's P1 fields: stop recording first, so nothing miniprof
 * does from here on (gathers, writing the JSON) is counted. */
static void collect_parallel(rank_rec_t *r, double elapsed_s, mpi_snap_t *snap)
{
    io_snap_t io;
    io_trace_get(&io);
    io_trace_set_enabled(0);
    mpi_trace_snapshot_get(snap);
    mpi_trace_set_mode(MPI_TRACE_OFF);

    ompt_snapshot_end();
    ompt_collect(&g_omp);
    r->omp_available  = g_omp.available;
    r->omp_threads    = g_omp.threads;
    r->omp_parallel_s = g_omp.outer_parallel_ns * 1e-9;
    r->omp_work_s     = g_omp.work_ns * 1e-9;
    r->omp_wait_s     = g_omp.wait_ns * 1e-9;
    r->omp_lock_s     = g_omp.lock_ns * 1e-9;
    r->omp_taskwait_s = g_omp.taskwait_ns * 1e-9;
    r->omp_work_min_s = g_omp.work_min_ns * 1e-9;
    r->omp_work_max_s = g_omp.work_max_ns * 1e-9;

    cnt_stop();
    cnt_result_t cr;
    cnt_collect(&cr);
    r->cnt_available   = cr.available;
    r->cnt_n           = cr.n_active;
    r->cnt_threads     = cr.threads;
    r->cnt_dropped     = cr.threads_dropped;
    r->cnt_multiplexed = cr.multiplexed;
    r->cnt_rdpmc       = cr.rdpmc;
    r->cnt_reads_rdpmc   = cr.reads_rdpmc;
    r->cnt_reads_syscall = cr.reads_syscall;
    memcpy(r->cnt_total,   cr.total,   sizeof(cr.total));
    memcpy(r->cnt_compute, cr.compute, sizeof(cr.compute));
    memcpy(r->cnt_mpi,     cr.mpi,     sizeof(cr.mpi));
    g_cnt_group = cr.group;
    if (!cr.available) snprintf(g_cnt_reason, sizeof(g_cnt_reason), "%s", cr.reason);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#define TV(t) ((t).tv_sec + (t).tv_usec * 1e-6)
    r->cpu_user_s = TV(ru.ru_utime) - TV(g_ru0.ru_utime);
    r->cpu_sys_s  = TV(ru.ru_stime) - TV(g_ru0.ru_stime);
#undef TV
    r->max_rss_kb = ru.ru_maxrss;
    r->vcsw       = ru.ru_nvcsw  - g_ru0.ru_nvcsw;
    r->ivcsw      = ru.ru_nivcsw - g_ru0.ru_nivcsw;
    int ncpu = r->info.cpus_allowed > 0 ? r->info.cpus_allowed : 1;
    r->cpu_util = elapsed_s > 0
                ? (r->cpu_user_s + r->cpu_sys_s) / (elapsed_s * ncpu) : 0.0;
    int ncore = r->info.cores_allowed > 0 ? r->info.cores_allowed : ncpu;
    r->cpu_util_core = elapsed_s > 0
                ? (r->cpu_user_s + r->cpu_sys_s) / (elapsed_s * ncore) : 0.0;

    double mpi = 0.0;
    for (int f = 0; f < MPIF_COUNT; f++) {
        r->f_calls[f] = snap->calls[f];
        r->f_bytes[f] = snap->bytes[f];
        r->f_ns[f]    = snap->ns[f];
        double t = snap->ns[f] * 1e-9;
        r->class_s[mpi_trace_func_class(f)] += t;
        mpi += t;
    }
    r->io  = io;
    r->mpi_s = mpi;
    r->io_s  = (io.posix.read_ns + io.posix.write_ns + io.posix.meta_ns +
                io.mpiio.read_ns + io.mpiio.write_ns + io.mpiio.meta_ns) * 1e-9;
    r->compute_s = elapsed_s - r->mpi_s - r->io_s;
    if (r->compute_s < 0) r->compute_s = 0;
}

/* Reduce user regions across ranks. Ranks may define different regions:
 * rank 0 gathers every rank's names, forms the union (first-seen order,
 * capped at CNT_MAX_REGIONS), broadcasts it, and every rank then reduces
 * its values aligned to the union. Collective over MPI_COMM_WORLD. */
static void reduce_regions(int rank, int size)
{
    static cnt_region_t mine[CNT_MAX_REGIONS];
    int n = cnt_regions_get(mine, CNT_MAX_REGIONS);

    int *counts = NULL, *displs = NULL;
    char *all = NULL, *mynames = NULL;
    if (rank == 0) {
        counts = calloc((size_t)size, sizeof(int));
        displs = calloc((size_t)size, sizeof(int));
    }
    PMPI_Gather(&n, 1, MPI_INT, counts, 1, MPI_INT, 0, MPI_COMM_WORLD);

    int total = 0;
    if (rank == 0 && counts && displs)
        for (int i = 0; i < size; i++) {
            counts[i] *= CNT_REGION_NAME;
            displs[i] = total;
            total += counts[i];
        }
    if (rank == 0) all = malloc((size_t)(total > 0 ? total : 1));
    mynames = malloc((size_t)(n > 0 ? n : 1) * CNT_REGION_NAME);
    for (int i = 0; i < n; i++) memcpy(mynames + (size_t)i * CNT_REGION_NAME, mine[i].name, CNT_REGION_NAME);
    PMPI_Gatherv(mynames, n * CNT_REGION_NAME, MPI_CHAR, all, counts, displs,
                 MPI_CHAR, 0, MPI_COMM_WORLD);

    static char uni[CNT_MAX_REGIONS][CNT_REGION_NAME];
    int nu = 0;
    if (rank == 0 && all) {
        for (int off = 0; off + CNT_REGION_NAME <= total && nu < CNT_MAX_REGIONS;
             off += CNT_REGION_NAME) {
            const char *nm = all + off;
            int dup = 0;
            for (int u = 0; u < nu && !dup; u++) dup = strncmp(uni[u], nm, CNT_REGION_NAME) == 0;
            if (!dup) memcpy(uni[nu++], nm, CNT_REGION_NAME);
        }
    }
    PMPI_Bcast(&nu, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (nu > 0) PMPI_Bcast(uni, nu * CNT_REGION_NAME, MPI_CHAR, 0, MPI_COMM_WORLD);

    /* per union entry: [present, calls, ns, ev...] summed; ns min/max */
    enum { K = 3 + CNT_MAX_EVENTS };
    static uint64_t sum_in[CNT_MAX_REGIONS][K], sum_out[CNT_MAX_REGIONS][K];
    static uint64_t min_in[CNT_MAX_REGIONS], min_out[CNT_MAX_REGIONS];
    static uint64_t max_in[CNT_MAX_REGIONS], max_out[CNT_MAX_REGIONS];
    memset(sum_in, 0, sizeof(sum_in));
    for (int u = 0; u < nu; u++) { min_in[u] = UINT64_MAX; max_in[u] = 0; }
    for (int u = 0; u < nu; u++)
        for (int i = 0; i < n; i++)
            if (strncmp(uni[u], mine[i].name, CNT_REGION_NAME) == 0) {
                sum_in[u][0] = 1;
                sum_in[u][1] = mine[i].calls;
                sum_in[u][2] = mine[i].ns;
                for (int e = 0; e < CNT_MAX_EVENTS; e++) sum_in[u][3 + e] = mine[i].ev[e];
                min_in[u] = max_in[u] = mine[i].ns;
            }
    if (nu > 0) {
        PMPI_Reduce(sum_in, sum_out, nu * K, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);
        PMPI_Reduce(min_in, min_out, nu, MPI_UINT64_T, MPI_MIN, 0, MPI_COMM_WORLD);
        PMPI_Reduce(max_in, max_out, nu, MPI_UINT64_T, MPI_MAX, 0, MPI_COMM_WORLD);
    }
    if (rank == 0) {
        g_nrreg = nu;
        for (int u = 0; u < nu; u++) {
            region_red_t *r = &g_rreg[u];
            memcpy(r->name, uni[u], CNT_REGION_NAME);
            r->name[CNT_REGION_NAME - 1] = '\0';
            r->ranks  = sum_out[u][0];
            r->calls  = sum_out[u][1];
            r->ns_sum = sum_out[u][2];
            r->ns_min = min_out[u];
            r->ns_max = max_out[u];
            for (int e = 0; e < CNT_MAX_EVENTS; e++) r->ev[e] = sum_out[u][3 + e];
        }
    }
    free(counts); free(displs); free(all); free(mynames);
}

static void omp_key(const void *codeptr, char *key)
{
    Dl_info di;
    memset(&di, 0, sizeof(di));
    if (codeptr && dladdr(codeptr, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        snprintf(key, OMP_KEY, "%.40s+0x%lx", b ? b + 1 : di.dli_fname,
                 (unsigned long)((const char *)codeptr - (const char *)di.dli_fbase));
    } else {
        snprintf(key, OMP_KEY, "%p", codeptr);
    }
}

/* Union of OpenMP regions across ranks (same protocol as user regions). */
static void reduce_omp_regions(int rank, int size)
{
    int n = g_omp.available ? g_omp.nregions : 0;
    static char mykeys[OMP_MAX_REGIONS][OMP_KEY];
    for (int i = 0; i < n; i++) omp_key(g_omp.regions[i].codeptr, mykeys[i]);

    int *counts = NULL, *displs = NULL;
    char *all = NULL;
    if (rank == 0) { counts = calloc((size_t)size, sizeof(int)); displs = calloc((size_t)size, sizeof(int)); }
    PMPI_Gather(&n, 1, MPI_INT, counts, 1, MPI_INT, 0, MPI_COMM_WORLD);
    int total = 0;
    if (rank == 0 && counts && displs)
        for (int i = 0; i < size; i++) { counts[i] *= OMP_KEY; displs[i] = total; total += counts[i]; }
    if (rank == 0) all = malloc((size_t)(total > 0 ? total : 1));
    PMPI_Gatherv(mykeys, n * OMP_KEY, MPI_CHAR, all, counts, displs, MPI_CHAR, 0, MPI_COMM_WORLD);

    static char uni[OMP_MAX_REGIONS][OMP_KEY];
    int nu = 0;
    if (rank == 0 && all)
        for (int off = 0; off + OMP_KEY <= total && nu < OMP_MAX_REGIONS; off += OMP_KEY) {
            int dup = 0;
            for (int u = 0; u < nu && !dup; u++) dup = strncmp(uni[u], all + off, OMP_KEY) == 0;
            if (!dup) memcpy(uni[nu++], all + off, OMP_KEY);
        }
    PMPI_Bcast(&nu, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (nu > 0) PMPI_Bcast(uni, nu * OMP_KEY, MPI_CHAR, 0, MPI_COMM_WORLD);

    enum { K = 6 };   /* present, calls, wall, task, wait, tasks */
    static uint64_t sin[OMP_MAX_REGIONS][K], sout[OMP_MAX_REGIONS][K];
    static uint64_t min_[OMP_MAX_REGIONS], mout[OMP_MAX_REGIONS];
    memset(sin, 0, sizeof(sin));
    for (int u = 0; u < nu; u++) min_[u] = 0;
    for (int u = 0; u < nu; u++)
        for (int i = 0; i < n; i++)
            if (strncmp(uni[u], mykeys[i], OMP_KEY) == 0) {
                const omp_region_t *r = &g_omp.regions[i];
                sin[u][0] = 1; sin[u][1] = r->calls; sin[u][2] = r->wall_ns;
                sin[u][3] = r->task_ns; sin[u][4] = r->wait_ns; sin[u][5] = r->tasks;
                min_[u] = r->max_team;
            }
    if (nu > 0) {
        PMPI_Reduce(sin, sout, nu * K, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);
        PMPI_Reduce(min_, mout, nu, MPI_UINT64_T, MPI_MAX, 0, MPI_COMM_WORLD);
    }
    if (rank == 0) {
        g_nored = nu;
        /* names: resolve rank 0's own construct addresses (one batch) */
        static miniprof_sym_t syms[OMP_MAX_REGIONS];
        int map[OMP_MAX_REGIONS], ns = 0;
        for (int u = 0; u < nu; u++) {
            map[u] = -1;
            for (int i = 0; i < n; i++)
                if (strncmp(uni[u], mykeys[i], OMP_KEY) == 0) {
                    memset(&syms[ns], 0, sizeof(syms[ns]));
                    /* return address → the call site is the byte before */
                    syms[ns].pc = (char *)g_omp.regions[i].codeptr - 1;
                    map[u] = ns++;
                    break;
                }
        }
        if (ns > 0) symbolizer_resolve(syms, ns, 1);
        for (int u = 0; u < nu; u++) {
            omp_red_t *o = &g_ored[u];
            memset(o, 0, sizeof(*o));
            memcpy(o->key, uni[u], OMP_KEY);
            o->key[OMP_KEY - 1] = '\0';
            if (map[u] >= 0) {
                snprintf(o->name, sizeof(o->name), "%s", syms[map[u]].name);
                snprintf(o->file, sizeof(o->file), "%s", syms[map[u]].file);
                o->line = syms[map[u]].line;
            }
            o->ranks = sout[u][0]; o->calls = sout[u][1]; o->wall_ns = sout[u][2];
            o->task_ns = sout[u][3]; o->wait_ns = sout[u][4]; o->tasks = sout[u][5];
            o->max_team = mout[u];
        }
    }
    free(counts); free(displs); free(all);
}

void snapshot_finalize(double elapsed_s)
{
    double t0 = now_s();

    rank_rec_t mine;
    memset(&mine, 0, sizeof(mine));
    mine.info = g_rank;
    mpi_snap_t snap;
    collect_parallel(&mine, elapsed_s, &snap);

    int rank = 0, size = 1;
    PMPI_Comm_rank(MPI_COMM_WORLD, &rank);
    PMPI_Comm_size(MPI_COMM_WORLD, &size);

    /* node topology */
    MPI_Comm node_comm, leader_comm;
    PMPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                         MPI_INFO_NULL, &node_comm);
    int node_rank = 0, node_size = 1;
    PMPI_Comm_rank(node_comm, &node_rank);
    PMPI_Comm_size(node_comm, &node_size);

    int is_leader = (node_rank == 0);
    PMPI_Comm_split(MPI_COMM_WORLD, is_leader ? 0 : MPI_UNDEFINED, rank,
                    &leader_comm);

    int node_index = 0, nnodes = 0;
    if (is_leader) {
        PMPI_Comm_rank(leader_comm, &node_index);
        PMPI_Comm_size(leader_comm, &nnodes);
    }
    PMPI_Bcast(&node_index, 1, MPI_INT, 0, node_comm);

    mine.rank       = rank;
    mine.node_index = node_index;
    mine.elapsed_s  = elapsed_s;
    mine.init_s     = g_init_s;

    /* message-size histograms: summed over ranks, only rank 0 needs them */
    static mpi_snap_t hist_sum;   /* static: ~2 KB, keep it off the stack */
    memset(&hist_sum, 0, sizeof(hist_sum));
    PMPI_Reduce(snap.hist_calls, hist_sum.hist_calls, MPIC_COUNT * MPI_HIST_BINS,
                MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);
    PMPI_Reduce(snap.hist_ns, hist_sum.hist_ns, MPIC_COUNT * MPI_HIST_BINS,
                MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);

    reduce_regions(rank, size);
    reduce_omp_regions(rank, size);

    mine.finalize_s = now_s() - t0;

    rank_rec_t *all_ranks = NULL;
    if (rank == 0) {
        all_ranks = calloc((size_t)size, sizeof(rank_rec_t));
        if (!all_ranks) {
            fprintf(stderr, "[miniprof] snapshot: out of memory gathering %d ranks\n", size);
        }
    }
    /* Every rank must take part in the gather even if rank 0's calloc
     * failed; rank 0 then just receives into a 1-element scratch. */
    rank_rec_t scratch;
    PMPI_Gather(&mine, (int)sizeof(mine), MPI_BYTE,
                all_ranks ? (void *)all_ranks : (void *)&scratch,
                (int)sizeof(mine), MPI_BYTE, 0, MPI_COMM_WORLD);

    node_rec_t *all_nodes = NULL;
    if (is_leader) {
        node_rec_t nmine;
        memset(&nmine, 0, sizeof(nmine));
        nmine.node_index       = node_index;
        nmine.ranks_on_node    = node_size;
        nmine.first_world_rank = rank;
        nmine.info             = g_node;
        if (rank == 0) all_nodes = calloc((size_t)nnodes, sizeof(node_rec_t));
        node_rec_t nscratch;
        PMPI_Gather(&nmine, (int)sizeof(nmine), MPI_BYTE,
                    all_nodes ? (void *)all_nodes : (void *)&nscratch,
                    (int)sizeof(nmine), MPI_BYTE, 0, leader_comm);
        PMPI_Comm_free(&leader_comm);
    }
    PMPI_Comm_free(&node_comm);

    if (rank == 0 && all_ranks && all_nodes) {
        char lib[MPI_MAX_LIBRARY_VERSION_STRING];
        int len = 0;
        lib[0] = '\0';
        if (PMPI_Get_library_version(lib, &len) == MPI_SUCCESS) {
            /* first line only — some libraries append build details */
            char *nl = strchr(lib, '\n');
            if (nl) *nl = '\0';
        }
        double finalize_s = now_s() - t0;
        if (write_json(all_ranks, size, all_nodes, nnodes, lib, finalize_s,
                       &hist_sum) == 0
            && g_verbose) {
            fprintf(stderr, "[miniprof] snapshot written to %s (%d ranks, %d nodes)\n",
                    g_outfile, size, nnodes);
        }
    }
    free(all_ranks);
    free(all_nodes);
}
