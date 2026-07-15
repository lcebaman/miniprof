/*
 * trace_store.c — accumulate per-function call/timing stats from
 * __cyg_profile_func_enter/exit hooks (see instrument.c)
 *
 * Design:
 *   - A small open-addressing hash table keyed by function PC, storing
 *     calls / inclusive time / exclusive time. Sized for a few hundred
 *     traced functions (the .mpa-selected set), not the whole program.
 *   - A thread-local call stack tracks entry timestamps so we can compute
 *     both inclusive time (entry→exit) and exclusive time (inclusive minus
 *     time spent in children, attributed back to the parent on each pop).
 *
 * Concurrency note: like profile_store, this must be safe to call from
 * arbitrary points in the user's code (every instrumented function call),
 * so we keep operations branch-light and avoid locks via atomics. Single
 * MPI rank = single OS process; we assume one thread calls into this for
 * the common MPI-only case. If the target uses OpenMP threads as well,
 * the call stack is made thread-local so nesting stays correct per-thread,
 * while the shared hash table uses atomic increments for safety.
 */

#include "trace_store.h"
#include "sampler_hwc.h"
#include "../include/miniprof.h"
#include <string.h>
#include <stdatomic.h>
#include <time.h>

/* ── hash table slot ─────────────────────────────────────────────────────── */
typedef struct {
    _Atomic(uintptr_t) pc;          /* 0 = empty                            */
    _Atomic(long)       calls;
    double               incl_time_s; /* plain double: see file header note
                                          re: single-thread-per-rank assumption */
    double               excl_time_s;
    /* inclusive hardware-counter totals (0/0/0 if unavailable) — see the
       trace_store_iterate() doc comment in trace_store.h */
    uint64_t             cycles;
    uint64_t             instructions;
    uint64_t             cache_misses;
} trace_slot_t;

#define TRACE_TABLE_CAPACITY 1024   /* power of 2, >= MINIPROF_TRACE_MAX_FUNCS*2 */
static trace_slot_t g_table[TRACE_TABLE_CAPACITY];

/* ── thread-local call stack for inclusive/exclusive timing ─────────────── */
typedef struct {
    uintptr_t pc;
    double    t_enter;
    double    child_time;          /* time already attributed to children */
    double    mpi_time_at_enter;   /* snapshot of g_mpi_time_total on entry */
    hwc_snapshot_t hwc_enter;      /* zeroed if hardware counters unavailable */
} frame_t;

static _Thread_local frame_t g_stack[MINIPROF_TRACE_STACK_DEPTH];
static _Thread_local int     g_sp = 0;   /* stack pointer */

/*
 * Running total of time spent inside MPI calls, bumped by mpi_trace.c
 * after every PMPI_* call returns. Used to correctly exclude MPI time
 * from a traced user function's "exclusive" (self) time, since libmpi.so
 * is never built with -finstrument-functions and so never participates
 * in the child_time bookkeeping above on its own.
 *
 * Thread-local to match the call-stack: each thread's MPI time should
 * only offset that same thread's traced frames. (MPI calls themselves
 * are usually made from a single thread per rank in MPI+OpenMP codes
 * unless MPI_THREAD_MULTIPLE is used, but thread-local keeps this
 * correct either way without extra synchronization.)
 */
static _Thread_local double g_mpi_time_total = 0.0;

double trace_store_mpi_time_total(void) { return g_mpi_time_total; }
void   trace_store_add_mpi_time(double seconds) { g_mpi_time_total += seconds; }

/* ── wall clock helper ───────────────────────────────────────────────────── */
static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* FNV-1a on a pointer value (same scheme as profile_store) */
static inline uint32_t hash_pc(uintptr_t pc)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < (int)sizeof(uintptr_t); i++) {
        h ^= (uint8_t)(pc & 0xff);
        h *= 16777619u;
        pc >>= 8;
    }
    return h;
}

void trace_store_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    g_sp = 0;
    g_mpi_time_total = 0.0;
}

/* find-or-create a slot for `pc`; returns slot index, or -1 if table full */
static int find_or_create_slot(uintptr_t pc)
{
    uint32_t idx = hash_pc(pc) & (TRACE_TABLE_CAPACITY - 1);
    for (int probe = 0; probe < TRACE_TABLE_CAPACITY; probe++) {
        uintptr_t slot_pc = atomic_load_explicit(&g_table[idx].pc,
                                                 memory_order_relaxed);
        if (slot_pc == pc) return (int)idx;
        if (slot_pc == 0) {
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &g_table[idx].pc, &expected, pc,
                    memory_order_relaxed, memory_order_relaxed)) {
                return (int)idx;
            }
            slot_pc = atomic_load_explicit(&g_table[idx].pc,
                                           memory_order_relaxed);
            if (slot_pc == pc) return (int)idx;
        }
        idx = (idx + 1) & (TRACE_TABLE_CAPACITY - 1);
    }
    return -1; /* table full */
}

/*
 * Called from __cyg_profile_func_enter (via instrument.c) only for
 * functions the runtime filter has decided to trace.
 */
void trace_store_enter(void *pc)
{
    if (g_sp >= MINIPROF_TRACE_STACK_DEPTH) return; /* stack overflow guard */
    g_stack[g_sp].pc                = (uintptr_t)pc;
    g_stack[g_sp].t_enter           = now_s();
    g_stack[g_sp].child_time        = 0.0;
    g_stack[g_sp].mpi_time_at_enter = g_mpi_time_total;
    if (hwc_enabled()) {
        if (hwc_read(&g_stack[g_sp].hwc_enter) != 0)
            memset(&g_stack[g_sp].hwc_enter, 0, sizeof(hwc_snapshot_t));
    } else {
        memset(&g_stack[g_sp].hwc_enter, 0, sizeof(hwc_snapshot_t));
    }
    g_sp++;
}

void trace_store_exit(void *pc)
{
    if (g_sp <= 0) return; /* unmatched exit (e.g. longjmp) — ignore */
    g_sp--;
    frame_t *fr = &g_stack[g_sp];

    /* tolerate mismatched enter/exit due to longjmp/exceptions by
       walking down to find a matching PC, else just use top of stack */
    if (fr->pc != (uintptr_t)pc) {
        int found = -1;
        for (int i = g_sp; i >= 0; i--) {
            if (g_stack[i].pc == (uintptr_t)pc) { found = i; break; }
        }
        if (found < 0) { g_sp++; return; } /* give up cleanly */
        g_sp = found;
        fr = &g_stack[g_sp];
    }

    double t_exit = now_s();
    double incl    = t_exit - fr->t_enter;

    /*
     * Time spent inside MPI calls made during this frame's lifetime is
     * child time too, even though libmpi.so itself never calls
     * trace_store_enter/exit (it isn't built with -finstrument-functions).
     * mpi_trace.c bumps a global running total every time an MPI call
     * returns; we snapshot that total on entry (see trace_store_enter)
     * and compare here, so the delta is exactly the MPI time incurred
     * while this frame was on the stack — including MPI calls made by
     * any nested traced functions, which is correct since their own
     * exclusive-time calculation already subtracted that same MPI time
     * from their own total before we ever see it as "child_time" below.
     */
    double mpi_delta = trace_store_mpi_time_total() - fr->mpi_time_at_enter;

    double excl = incl - fr->child_time - mpi_delta;
    if (excl < 0.0) excl = 0.0;

    /* attribute this frame's inclusive time to its parent's child_time */
    if (g_sp > 0) g_stack[g_sp - 1].child_time += incl;

    /*
     * Hardware counters: exact entry-to-exit delta for this one
     * invocation — inclusive of any callees, unlike excl_time_s above.
     * Unlike sample mode's statistical leaf-PC attribution, this is
     * exact per-call accounting, at the cost of only covering the
     * functions actually named in the .mpa trace list.
     */
    uint64_t dcyc = 0, dins = 0, dmiss = 0;
    if (hwc_enabled()) {
        hwc_snapshot_t hwc_exit;
        if (hwc_read(&hwc_exit) == 0)
            hwc_delta(&fr->hwc_enter, &hwc_exit, &dcyc, &dins, &dmiss);
    }

    int slot = find_or_create_slot((uintptr_t)pc);
    if (slot < 0) return; /* table full — drop sample */

    atomic_fetch_add_explicit(&g_table[slot].calls, 1L, memory_order_relaxed);

    /*
     * Plain (non-atomic) read-modify-write on the double/counter fields.
     * This is safe under the single-thread-per-MPI-rank assumption
     * documented in the file header. For OpenMP+MPI builds with
     * concurrent calls into the *same* traced function from multiple
     * threads, this is a known approximation (rare lost updates under
     * heavy contention) — acceptable for a lightweight profiler; a
     * future version could add a per-thread slot table merged at
     * finalize time if needed.
     */
    g_table[slot].incl_time_s += incl;
    g_table[slot].excl_time_s += excl;
    g_table[slot].cycles       += dcyc;
    g_table[slot].instructions += dins;
    g_table[slot].cache_misses += dmiss;
}

void trace_store_iterate(void (*cb)(uintptr_t pc, long calls,
                                    double incl_s, double excl_s,
                                    uint64_t cycles, uint64_t instructions,
                                    uint64_t cache_misses, void *arg),
                         void *arg)
{
    for (int i = 0; i < TRACE_TABLE_CAPACITY; i++) {
        uintptr_t pc = atomic_load(&g_table[i].pc);
        if (!pc) continue;
        long     calls = atomic_load(&g_table[i].calls);
        double   incl   = g_table[i].incl_time_s;
        double   excl   = g_table[i].excl_time_s;
        uint64_t cycles = g_table[i].cycles;
        uint64_t insns  = g_table[i].instructions;
        uint64_t misses = g_table[i].cache_misses;
        if (calls > 0) cb(pc, calls, incl, excl, cycles, insns, misses, arg);
    }
}
