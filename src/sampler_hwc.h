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

/* Opens the CYCLES/INSTRUCTIONS/CACHE_MISSES counter group for the
 * calling thread and enables it. Never fails the caller: on any error
 * (EACCES/EPERM from perf_event_paranoid, ENOENT/ENOSYS on a VM without
 * a virtual PMU, or partial counter support) it prints a diagnostic to
 * stderr and leaves hardware counters disabled — sampling continues
 * either way. Returns 1 if counters are active, 0 otherwise. */
int hwc_init(void);

/* 1 if hwc_init() successfully enabled the counter group. */
int hwc_enabled(void);

/* Single read() of the counter group — async-signal-safe, safe to call
 * from the SIGPROF handler. Writes *out and returns 0 on success, -1 on
 * a short/failed read. No-op (returns -1) if hwc_enabled() is false. */
int hwc_read(hwc_snapshot_t *out);

/* Closes the counter fds. Safe to call even if hwc_init() was never
 * called or failed. */
void hwc_shutdown(void);

#endif
