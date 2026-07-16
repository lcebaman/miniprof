#ifndef SAMPLER_HWC_H
#define SAMPLER_HWC_H

#include <stdint.h>

/* One atomic snapshot of the counter group, as returned by a single
 * read() on the group leader fd (PERF_FORMAT_GROUP). time_enabled/
 * time_running let the caller detect and correct for PMU multiplexing. */
typedef struct {
    uint64_t nr;              /* number of counters in the group (3)   */
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t cycles;
    uint64_t instructions;
    uint64_t cache_misses;
} hwc_snapshot_t;

/*
 * Every counter group below is PER OS THREAD (perf_event_open with
 * pid=0 only ever counts the calling thread). hwc_read()/hwc_read_delta()
 * transparently open the calling thread's own group the first time
 * that thread needs it — an OpenMP worker thread that never calls
 * hwc_init() still gets correct, independent counters the first time
 * SIGPROF fires on it or it enters a traced function. hwc_init() is
 * just the eager, diagnostic-printing variant for the first thread
 * (typically the MPI_Init caller), so paranoid/unsupported systems get
 * one clear stderr message instead of silent per-thread no-ops.
 */

/* Opens the CYCLES/INSTRUCTIONS/CACHE_MISSES counter group for the
 * calling thread and enables it. Never fails the caller: on any error
 * (EACCES/EPERM from perf_event_paranoid, ENOENT/ENOSYS on a VM without
 * a virtual PMU, or partial counter support) it prints a diagnostic to
 * stderr (once, rank 0 only) and leaves this thread's counters disabled
 * — sampling continues either way. Returns 1 if counters are active for
 * this thread, 0 otherwise. Idempotent: a second call on the same
 * thread just returns the first call's result. */
int hwc_init(void);

/* 1 if hwc_init() successfully enabled counters for the thread that
 * called it (typically the MPI_Init caller) — used for the rank-level
 * "hardware counters available" summary in reports. Says nothing about
 * any other thread's counters, which succeed or fail independently. */
int hwc_enabled(void);

/* Single read() of the CALLING thread's counter group — async-signal-
 * safe, safe to call from the SIGPROF handler or from inside a traced
 * function on any thread. Lazily opens this thread's group on first
 * use if it doesn't have one yet (silently — no stdio/MPI calls on
 * that path, so this is safe even the very first time it's reached
 * from within a signal handler). Writes *out and returns 0 on success,
 * -1 if this thread's counters are unavailable or the read failed. */
int hwc_read(hwc_snapshot_t *out);

/* Delta since the last call *on this thread* (or since this thread's
 * group was opened, for the first call), corrected for PMU multiplexing
 * via time_enabled/time_running. Async-signal-safe — this is what the
 * SIGPROF handler calls. Returns 0 and writes *cycles, *instructions,
 * *cache_misses on success, -1 (leaving the outputs untouched) if this
 * thread's counters are unavailable or the read failed. */
int hwc_read_delta(uint64_t *cycles, uint64_t *instructions,
                   uint64_t *cache_misses);

/* Pure delta between two arbitrary snapshots (e.g. one taken at a traced
 * function's entry, one at its exit), with the same multiplexing
 * correction as hwc_read_delta() but no internal state — callers own
 * both snapshots. Safe to call from anywhere, including signal handlers
 * (no I/O, just arithmetic). */
void hwc_delta(const hwc_snapshot_t *before, const hwc_snapshot_t *after,
              uint64_t *cycles, uint64_t *instructions,
              uint64_t *cache_misses);

/* Closes the CALLING thread's counter fds only — that's the only state
 * reachable from here. Other threads' groups are closed implicitly by
 * the OS when those threads exit or the process terminates. Safe to
 * call even if hwc_init() was never called or failed on this thread. */
void hwc_shutdown(void);

#endif
