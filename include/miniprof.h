#ifndef MINIPROF_H
#define MINIPROF_H

#include <stdint.h>

/*
 * miniprof.h — public API for the miniprof sampling/tracing profiler
 *
 * Zero-config usage:
 *   Link with -lminiprof -ldl -lmpi
 *   MPI_Init / MPI_Finalize are intercepted via PMPI wrappers.
 *
 * Manual usage (non-MPI or finer control):
 *   miniprof_init(interval_ms);
 *   ... your code ...
 *   miniprof_finalize();
 *
 * Modes, selected by MINIPROF_MODE:
 *
 *   sample (default) — statistical SIGPROF sampling, as before.
 *                       Produces miniprof_report.txt AND miniprof.mpa
 *                       (a suggested function list for trace mode).
 *
 *   trace             — exact entry/exit instrumentation of the
 *                       functions named in MINIPROF_TRACE_FUNCS
 *                       (defaults to miniprof.mpa from a prior sampling
 *                       run), plus per-call MPI timing via PMPI.
 *                       Requires building the target with
 *                       -finstrument-functions (see `make example-trace`).
 *                       Produces miniprof_trace_report.txt.
 *
 *   snapshot          — whole-run characterisation (APS/uProfPcm style):
 *                       running totals only, written as one versioned
 *                       JSON document at MPI_Finalize. No rebuild needed.
 *
 *   off               — library loaded but inactive (baseline runs).
 *
 * Environment variables:
 *   MINIPROF_MODE          sample|trace|snapshot|off   (default: sample)
 *   MINIPROF_SNAPSHOT      basic|topdown|roofline|full (default: basic)
 *   MINIPROF_SNAPSHOT_OUTPUT Snapshot JSON path   (default: miniprof_snapshot.json)
 *   MINIPROF_INTERVAL_MS   Sampling interval in ms     (default: 10)
 *   MINIPROF_DEPTH         Call-stack depth to sample  (default: 8)
 *   MINIPROF_OUTPUT        Sampling report path        (default: miniprof_report.txt)
 *   MINIPROF_MPA_OUTPUT    MPA suggestions file path   (default: miniprof.mpa)
 *   MINIPROF_MPA_THRESHOLD Min Samp% to suggest tracing (default: 1.0)
 *   MINIPROF_TRACE_FUNCS   Function list for trace mode (default: miniprof.mpa)
 *   MINIPROF_TRACE_OUTPUT  Tracing report path          (default: miniprof_trace_report.txt)
 *   MINIPROF_VERBOSE       Print per-rank diagnostics   (default: 0)
 *   MINIPROF_IPC_COMPUTE_THRESHOLD  IPC above this => COMPUTE-BOUND (default: 1.2)
 *   MINIPROF_IPC_MEMORY_THRESHOLD   IPC below this + high LLC misses
 *                                   => MEMORY-BOUND                 (default: 0.5)
 *   MINIPROF_MPKI_MEMORY_THRESHOLD  MPKI (LLC misses per 1000 instructions)
 *                                   above this, combined with low IPC,
 *                                   => MEMORY-BOUND                 (default: 10)
 */

#ifdef __cplusplus
extern "C" {
#endif

#define MINIPROF_VERSION "1.1.0-dev"

/* ── user regions (snapshot mode) ─────────────────────────────────────────
 * Bracket a code region to get its time and hardware counters reported
 * separately in the snapshot JSON (metrics.counters.regions). Regions nest
 * (up to 16 deep per thread) and are inclusive; up to 64 distinct names.
 * No-ops outside snapshot mode, so they can stay in production code.
 * Fortran: CALL MINIPROF_REGION_BEGIN('name') / MINIPROF_REGION_END('name').
 */
void miniprof_region_begin(const char *name);
void miniprof_region_end(const char *name);

/* ── tunable defaults (overridden by env vars) ───────────────────────────── */
#define MINIPROF_DEFAULT_INTERVAL_MS  10
#define MINIPROF_DEFAULT_DEPTH         8
#define MINIPROF_DEFAULT_OUTPUT       "miniprof_report.txt"
#define MINIPROF_DEFAULT_MPA_OUTPUT   "miniprof.mpa"
#define MINIPROF_DEFAULT_MPA_THRESHOLD 1.0
#define MINIPROF_DEFAULT_TRACE_FUNCS   "miniprof.mpa"
#define MINIPROF_DEFAULT_TRACE_OUTPUT  "miniprof_trace_report.txt"

/* ── hardware-counter classification thresholds (Tables 2 and 3) ────────── */
#define MINIPROF_DEFAULT_IPC_COMPUTE_THRESHOLD  1.2   /* IPC above → COMPUTE-BOUND */
#define MINIPROF_DEFAULT_IPC_MEMORY_THRESHOLD   0.5   /* IPC below → candidate MEMORY-BOUND */
#define MINIPROF_DEFAULT_MPKI_MEMORY_THRESHOLD 10.0   /* + MPKI above → MEMORY-BOUND */

/* ── profile store limits ────────────────────────────────────────────────── */
#define MINIPROF_MAX_PCS        65536   /* unique PCs per rank              */
#define MINIPROF_MAX_DEPTH      32      /* hard cap on backtrace depth       */
#define MINIPROF_SYMBOL_LEN     128     /* max symbol/DSO name length        */
#define MINIPROF_TOP_N          40      /* functions shown in flat profile   */
#define MINIPROF_TOP_PER_RANK    5      /* symbols shown per rank            */
#define MINIPROF_MAX_SEND_SYMS 2048     /* (function,line) entries sent/rank */
#define MINIPROF_GROUP_LEN       16     /* group label, e.g. "USER","MPI"    */
#define MINIPROF_MAX_LINES_PER_FUNC 8   /* line rows shown under a function  */

/* ── function groups ─────────────────────────────────────────────────────── */
#define MINIPROF_GROUP_USER    "USER"
#define MINIPROF_GROUP_MPI     "MPI"
#define MINIPROF_GROUP_OMP     "OMP"
#define MINIPROF_GROUP_LIBC    "LIBC"
#define MINIPROF_GROUP_SYS     "SYSTEM"
#define MINIPROF_GROUP_PROFILER "PROFILER"
#define MINIPROF_GROUP_OTHER   "ETC"

/* ── a single resolved symbol, aggregated at (function,file,line) granularity */
typedef struct {
    void       *pc;
    long        count;                       /* sample hit count             */
    char        name[MINIPROF_SYMBOL_LEN];   /* demangled symbol name        */
    char        dso [MINIPROF_SYMBOL_LEN];   /* shared object / exe name     */
    char        file[MINIPROF_SYMBOL_LEN];   /* source file (addr2line)      */
    int         line;                        /* source line  (addr2line)     */
    char        group[MINIPROF_GROUP_LEN];   /* USER / MPI / OMP / LIBC / .. */
    /* hardware-counter accumulation (0 if counters were unavailable) */
    uint64_t    cycles;
    uint64_t    instructions;
    uint64_t    cache_misses;
} miniprof_sym_t;

/* ── flat profile sent from each rank to rank 0 ──────────────────────────── */
typedef struct {
    int            rank;
    int            n_syms;                          /* entries used in syms[] */
    long           total_samples;
    double         wall_elapsed_s;
    int            hwc_available;    /* 1 if this rank had hardware counters */
    miniprof_sym_t syms[MINIPROF_MAX_SEND_SYMS];     /* full (func,line) table */
} miniprof_rank_profile_t;

/* ── public API ──────────────────────────────────────────────────────────── */
void miniprof_init(int interval_ms);
void miniprof_finalize(void);

/* ── trace-mode limits ────────────────────────────────────────────────────── */
#define MINIPROF_TRACE_MAX_FUNCS   512   /* distinct functions traced/rank    */
#define MINIPROF_TRACE_MAX_NAME    128   /* function name length in .mpa file */
#define MINIPROF_TRACE_STACK_DEPTH 256   /* max nesting depth per thread      */

/* one traced user function's accumulated stats (sent rank->rank0) */
typedef struct {
    char   name[MINIPROF_SYMBOL_LEN];
    long   calls;
    double incl_time_s;     /* inclusive wall time (incl. callees)           */
    double excl_time_s;     /* exclusive wall time (self only)               */
    /* exact per-invocation hardware counters, summed across calls —
       INCLUSIVE (like incl_time_s, not excl_time_s); 0 if unavailable */
    uint64_t cycles;
    uint64_t instructions;
    uint64_t cache_misses;
} miniprof_trace_func_t;

/* one traced MPI call's accumulated stats (sent rank->rank0) */
typedef struct {
    char   name[MINIPROF_SYMBOL_LEN];   /* e.g. "MPI_Allreduce"             */
    long   calls;
    double total_time_s;
    long   total_bytes;     /* sum of message sizes, where applicable        */
} miniprof_trace_mpi_t;

/* full per-rank trace profile, gathered to rank 0 */
typedef struct {
    int                     rank;
    double                  wall_elapsed_s;
    int                     n_funcs;
    int                     n_mpi;
    int                     hwc_available;   /* 1 if this rank had hardware counters */
    miniprof_trace_func_t   funcs[MINIPROF_TRACE_MAX_FUNCS];
    miniprof_trace_mpi_t    mpi  [MINIPROF_TRACE_MAX_FUNCS];
} miniprof_trace_profile_t;

#ifdef __cplusplus
}
#endif

#endif /* MINIPROF_H */
