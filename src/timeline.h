#ifndef MINIPROF_TIMELINE_H
#define MINIPROF_TIMELINE_H

/*
 * timeline.h — snapshot-mode time series per rank (P7, opt-in)
 *
 * MINIPROF_TIMELINE=1 (launcher: --timeline) splits the run into time
 * buckets and records, per rank:
 *   - time inside MPI calls and inside I/O calls (from the wrappers that
 *     already time every call; a call spanning buckets is split);
 *   - cumulative counter values of the rank's MPI thread (cycles,
 *     instructions, FLOPs) at its MPI-call boundaries, from the reads the
 *     counter engine already does there: the counts between two marks are
 *     spread over the buckets in between in proportion to time.
 * Buckets start at MINIPROF_TIMELINE_INTERVAL_MS (default 100 ms); when a
 * run outgrows TL_BUCKETS of them the width doubles and neighbours merge,
 * so any run length fits. Compute time = bucket width - MPI - I/O.
 *
 * Off (the default) costs one predictable branch per MPI/I/O call.
 */

#include <stdint.h>

#define TL_BUCKETS 512
#define TL_CNT     3        /* cycles, instructions, flops */

enum { TL_MPI = 0, TL_IO = 1 };

extern int tl_on;          /* read on the hot path: set once at init */

void tl_init(void);         /* reads MINIPROF_TIMELINE*, starts the clock */
void tl_add(int kind, uint64_t t_end_ns, uint64_t dt_ns);
/* cumulative counts of the MPI thread at time t; bit k of valid set if
 * cnt[k] (cycles, instructions, flops) is counted */
void tl_mark(uint64_t t_ns, const uint64_t cnt[TL_CNT], unsigned valid);
void tl_stop(void);         /* end of the run (start of finalize) */

typedef struct {
    int      enabled;
    uint64_t t0_ns, t1_ns;           /* CLOCK_MONOTONIC: start, end           */
    uint64_t width_ns;               /* bucket width                          */
    int      n;                      /* buckets in use                        */
    uint64_t mpi_ns[TL_BUCKETS], io_ns[TL_BUCKETS];
    /* first counter mark in each bucket */
    unsigned mark_valid[TL_BUCKETS]; /* 0 = no mark in this bucket            */
    uint64_t mark_t[TL_BUCKETS];
    uint64_t mark[TL_BUCKETS][TL_CNT];
    /* the run's last mark (at stop) */
    unsigned end_valid;
    uint64_t end_t, end_mark[TL_CNT];
} tl_data_t;

void tl_get(tl_data_t *out);

/* Rebin d to a coarser width (a power-of-two multiple of its own). */
void tl_rebin(tl_data_t *d, uint64_t width_ns);

#endif
