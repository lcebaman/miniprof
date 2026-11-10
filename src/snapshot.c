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
#include "../include/miniprof.h"

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

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
    long   max_rss_kb, vcsw, ivcsw;
    uint64_t f_calls[MPIF_COUNT], f_bytes[MPIF_COUNT], f_ns[MPIF_COUNT];
    io_snap_t io;
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
    jw_str_or_null(w, "cpus_allowed_list", rr->info.cpus_allowed_list);
    jw_dbl(w, "elapsed_s", rr->elapsed_s);
    jw_dbl(w, "compute_s", rr->compute_s);
    jw_dbl(w, "mpi_s", rr->mpi_s);
    jw_dbl(w, "io_s", rr->io_s);
    jw_dbl(w, "cpu_util_pct", 100.0 * rr->cpu_util);
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
    jw_stat(w, "user_s", stats(r, n, g_cpu_user, 0));
    jw_stat(w, "sys_s",  stats(r, n, g_cpu_sys, 0));
    jw_stat(w, "max_rss_mb", stats(r, n, g_rss_mb, 0));
    jw_int(w, "voluntary_ctx_switches", vcsw);
    jw_int(w, "involuntary_ctx_switches", ivcsw);
    jw_str(w, "note", "utilisation = CPU time / (elapsed x CPUs the rank may run on); "
                      "MPI libraries usually busy-poll while waiting, so MPI time counts as busy");
    jw_end_obj(w);

    jw_end_obj(w);   /* parallel */
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
    jw_end_arr(&w);
    write_parallel(&w, ranks, nranks, hist);
    jw_null(&w, "openmp");
    jw_null(&w, "counters");
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
