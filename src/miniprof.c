/*
 * miniprof.c — top-level init/finalize, mode switch, and PMPI wrappers
 *
 * Lifecycle:
 *   MPI_Init()  →  miniprof_init()   [PMPI wrapper]
 *   ... program runs ...
 *   MPI_Finalize() → miniprof_finalize() [PMPI wrapper]
 *
 * Two modes, selected by MINIPROF_MODE (default "sample"):
 *
 *   sample — SIGPROF statistical sampling (as before). At finalize:
 *            1. Stop sampler
 *            2. Symbolize sampled PCs, aggregate to (func,file,line)
 *            3. Gather via MPI + write miniprof_report.txt
 *            4. Write miniprof.mpa suggestions for trace mode
 *
 *   trace  — exact entry/exit instrumentation (-finstrument-functions)
 *            of the functions named in MINIPROF_TRACE_FUNCS, plus
 *            per-call MPI timing via mpi_trace.c's PMPI wrappers.
 *            At finalize:
 *            1. Collect trace_store + mpi_trace tables
 *            2. Gather via MPI + write miniprof_trace_report.txt
 */

#define _GNU_SOURCE
#include "../include/miniprof.h"
#include "sampler.h"
#include "symbolizer.h"
#include "aggregator.h"
#include "profile_store.h"
#include "instrument.h"
#include "trace_store.h"
#include "trace_aggregator.h"
#include "mpi_trace.h"

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* ── mode ─────────────────────────────────────────────────────────────────── */
typedef enum { MODE_SAMPLE, MODE_TRACE } miniprof_mode_t;

/* ── global state ────────────────────────────────────────────────────────── */
static int             g_initialized  = 0;
static miniprof_mode_t g_mode         = MODE_SAMPLE;
static int             g_interval_ms  = MINIPROF_DEFAULT_INTERVAL_MS;
static int             g_depth        = MINIPROF_DEFAULT_DEPTH;
static char            g_outfile[512]       = MINIPROF_DEFAULT_OUTPUT;
static char            g_mpa_outfile[512]   = MINIPROF_DEFAULT_MPA_OUTPUT;
static double          g_mpa_threshold      = MINIPROF_DEFAULT_MPA_THRESHOLD;
static char            g_trace_funcs[512]   = MINIPROF_DEFAULT_TRACE_FUNCS;
static char            g_trace_outfile[512] = MINIPROF_DEFAULT_TRACE_OUTPUT;
static int             g_verbose      = 0;
static struct timespec g_t_start;

/* ── env-var helpers ─────────────────────────────────────────────────────── */
static int env_int(const char *var, int def)
{
    const char *v = getenv(var);
    return v ? atoi(v) : def;
}
static double env_double(const char *var, double def)
{
    const char *v = getenv(var);
    return v ? atof(v) : def;
}

/* ── sample-mode scratch buffer ──────────────────────────────────────────── */
typedef struct { uintptr_t pc; long count; } pc_count_t;
static pc_count_t g_pcbuf[MINIPROF_TOP_N * 8];
static int        g_pcbuf_n = 0;

static int collect_cb(uintptr_t pc, long count, void *arg)
{
    (void)arg;
    if (g_pcbuf_n < (int)(sizeof(g_pcbuf)/sizeof(g_pcbuf[0]))) {
        g_pcbuf[g_pcbuf_n].pc    = pc;
        g_pcbuf[g_pcbuf_n].count = count;
        g_pcbuf_n++;
    }
    return 0;
}

/* ── trace-mode scratch state, filled by callbacks from *_iterate() ─────── */
static miniprof_trace_profile_t g_trace_local;

static void trace_func_cb(uintptr_t pc, long calls,
                          double incl_s, double excl_s, void *arg)
{
    (void)arg;
    if (g_trace_local.n_funcs >= MINIPROF_TRACE_MAX_FUNCS) return;

    /* resolve name via instrument.c's nm-based table (not dladdr — see
       instrument.c file header for why dladdr can't see static functions) */
    miniprof_trace_func_t *fn = &g_trace_local.funcs[g_trace_local.n_funcs++];
    if (!instrument_lookup_name((void *)pc, fn->name, MINIPROF_SYMBOL_LEN))
        snprintf(fn->name, MINIPROF_SYMBOL_LEN, "%p", (void *)pc);
    fn->calls       = calls;
    fn->incl_time_s = incl_s;
    fn->excl_time_s = excl_s;
}

static void trace_mpi_cb(const char *name, long calls,
                         double total_s, long bytes, void *arg)
{
    (void)arg;
    if (g_trace_local.n_mpi >= MINIPROF_TRACE_MAX_FUNCS) return;
    miniprof_trace_mpi_t *m = &g_trace_local.mpi[g_trace_local.n_mpi++];
    snprintf(m->name, MINIPROF_SYMBOL_LEN, "%s", name);
    m->calls         = calls;
    m->total_time_s  = total_s;
    m->total_bytes   = bytes;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void miniprof_init(int interval_ms)
{
    if (g_initialized) return;
    g_initialized = 1;

    /* mode selection */
    const char *mode_s = getenv("MINIPROF_MODE");
    g_mode = (mode_s && strcmp(mode_s, "trace") == 0) ? MODE_TRACE : MODE_SAMPLE;

    g_verbose = env_int("MINIPROF_VERBOSE", 0);

    const char *out = getenv("MINIPROF_OUTPUT");
    if (out) snprintf(g_outfile, sizeof(g_outfile), "%s", out);

    const char *mpa_out = getenv("MINIPROF_MPA_OUTPUT");
    if (mpa_out) snprintf(g_mpa_outfile, sizeof(g_mpa_outfile), "%s", mpa_out);
    g_mpa_threshold = env_double("MINIPROF_MPA_THRESHOLD",
                                 MINIPROF_DEFAULT_MPA_THRESHOLD);

    const char *trace_funcs = getenv("MINIPROF_TRACE_FUNCS");
    if (trace_funcs) snprintf(g_trace_funcs, sizeof(g_trace_funcs), "%s", trace_funcs);

    const char *trace_out = getenv("MINIPROF_TRACE_OUTPUT");
    if (trace_out) snprintf(g_trace_outfile, sizeof(g_trace_outfile), "%s", trace_out);

    symbolizer_init();
    clock_gettime(CLOCK_MONOTONIC, &g_t_start);

    if (g_mode == MODE_SAMPLE) {
        g_interval_ms = env_int("MINIPROF_INTERVAL_MS", interval_ms > 0
                                                         ? interval_ms
                                                         : MINIPROF_DEFAULT_INTERVAL_MS);
        g_depth       = env_int("MINIPROF_DEPTH", MINIPROF_DEFAULT_DEPTH);
        profile_store_init();
        sampler_start(g_interval_ms, g_depth);

        if (g_verbose) {
            int rank; MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            fprintf(stderr, "[miniprof] rank %d: SAMPLE mode, every %d ms, "
                            "depth %d, output=%s, mpa=%s\n",
                    rank, g_interval_ms, g_depth, g_outfile, g_mpa_outfile);
        }
    } else {
        memset(&g_trace_local, 0, sizeof(g_trace_local));
        instrument_load_trace_list(g_trace_funcs);
        mpi_trace_set_enabled(1);

        if (g_verbose) {
            int rank; MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            fprintf(stderr, "[miniprof] rank %d: TRACE mode, "
                            "func list=%s, output=%s\n",
                    rank, g_trace_funcs, g_trace_outfile);
        }
    }
}

static void miniprof_finalize_sample(double elapsed)
{
    sampler_stop();

    g_pcbuf_n = 0;
    profile_store_iterate(collect_cb, NULL);

    int n_pcs = g_pcbuf_n < MINIPROF_TOP_N * 8 ? g_pcbuf_n : MINIPROF_TOP_N * 8;

    miniprof_sym_t *resolved = calloc(n_pcs, sizeof(miniprof_sym_t));
    for (int i = 0; i < n_pcs; i++) {
        resolved[i].pc    = (void *)(uintptr_t)g_pcbuf[i].pc;
        resolved[i].count = g_pcbuf[i].count;
    }
    symbolizer_resolve(resolved, n_pcs, /*use_addr2line=*/1);

    miniprof_rank_profile_t local;
    memset(&local, 0, sizeof(local));
    MPI_Comm_rank(MPI_COMM_WORLD, &local.rank);
    local.total_samples  = profile_store_total();
    local.wall_elapsed_s = elapsed;
    local.n_syms         = 0;

    for (int i = 0; i < n_pcs; i++) {
        int found = -1;
        for (int j = 0; j < local.n_syms; j++) {
            if (strcmp(local.syms[j].name, resolved[i].name) == 0 &&
                strcmp(local.syms[j].file, resolved[i].file) == 0 &&
                local.syms[j].line == resolved[i].line) {
                found = j; break;
            }
        }
        if (found >= 0) {
            local.syms[found].count += resolved[i].count;
        } else if (local.n_syms < MINIPROF_MAX_SEND_SYMS) {
            local.syms[local.n_syms] = resolved[i];
            local.n_syms++;
        }
    }
    free(resolved);

    if (g_verbose) {
        fprintf(stderr, "[miniprof] rank %d: %ld samples, "
                        "%.3f s wall, %d distinct (func,line) entries\n",
                local.rank, local.total_samples, elapsed, local.n_syms);
    }

    aggregator_run(&local, g_outfile, g_mpa_outfile, g_mpa_threshold);
}

static void miniprof_finalize_trace(double elapsed)
{
    MPI_Comm_rank(MPI_COMM_WORLD, &g_trace_local.rank);
    g_trace_local.wall_elapsed_s = elapsed;
    g_trace_local.n_funcs = 0;
    g_trace_local.n_mpi   = 0;

    trace_store_iterate(trace_func_cb, NULL);
    mpi_trace_iterate(trace_mpi_cb, NULL);

    if (g_verbose) {
        fprintf(stderr, "[miniprof] rank %d: TRACE finalize — "
                        "%d user functions, %d MPI functions, %.3f s wall\n",
                g_trace_local.rank, g_trace_local.n_funcs,
                g_trace_local.n_mpi, elapsed);
    }

    trace_aggregator_run(&g_trace_local, g_trace_outfile);
}

void miniprof_finalize(void)
{
    if (!g_initialized) return;
    g_initialized = 0;

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec  - g_t_start.tv_sec) +
                     (t_end.tv_nsec - g_t_start.tv_nsec) * 1e-9;

    if (g_mode == MODE_SAMPLE) miniprof_finalize_sample(elapsed);
    else                       miniprof_finalize_trace(elapsed);
}

/* ── PMPI wrappers ───────────────────────────────────────────────────────── */
/*
 * These intercept MPI_Init / MPI_Finalize without requiring the user
 * to modify their source code.  Link with -lminiprof before -lmpi.
 */

int MPI_Init(int *argc, char ***argv)
{
    int rc = PMPI_Init(argc, argv);
    if (rc == MPI_SUCCESS)
        miniprof_init(MINIPROF_DEFAULT_INTERVAL_MS);
    return rc;
}

int MPI_Init_thread(int *argc, char ***argv, int required, int *provided)
{
    int rc = PMPI_Init_thread(argc, argv, required, provided);
    if (rc == MPI_SUCCESS)
        miniprof_init(MINIPROF_DEFAULT_INTERVAL_MS);
    return rc;
}

int MPI_Finalize(void)
{
    miniprof_finalize();
    return PMPI_Finalize();
}

/*
 * Fortran PMPI binding wrappers (mpi_init_, mpi_finalize_, etc.) live in
 * their own translation unit, miniprof_fortran.c, rather than here.
 *
 * Reason: a static archive (libminiprof.a) only pulls in a .o file when
 * something actually references a symbol defined in it. If the Fortran
 * wrappers were defined in this same file, linking ANY program against
 * libminiprof.a — including plain C programs that never call Fortran
 * MPI bindings — would pull in mpi_init_(), which references
 * pmpi_init_(). That symbol only exists in libmpi_mpifh.so (Open MPI's
 * Fortran binding shim), which mpicc does NOT link by default (only
 * mpifort does) — so every plain-C build would fail to link with an
 * "undefined reference to pmpi_init_" error, confirmed by testing.
 * Keeping the Fortran wrappers in a separate .o means the linker only
 * pulls that object — and its pmpi_*_ dependency — in when a Fortran
 * program (linked via mpifort, which does provide libmpi_mpifh) actually
 * references one of those symbols.
 */
