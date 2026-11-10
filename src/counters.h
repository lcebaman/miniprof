#ifndef MINIPROF_COUNTERS_H
#define MINIPROF_COUNTERS_H

/*
 * counters.h — snapshot-mode hardware counter engine (phase P3)
 *
 * Per-thread counting-mode perf groups, chosen from per-microarchitecture
 * event tables, read at three kinds of boundary:
 *   - MPI entry/exit (from mpi_trace.c): splits every thread's counts into
 *     "compute" (outside MPI) and "mpi" (inside MPI, mostly busy-polling)
 *   - user regions: miniprof_region_begin/end (C and Fortran)
 *   - finalize: the tail since the last boundary
 *
 * Reads use rdpmc (user-space, tens of ns) when the kernel allows it and
 * the group is currently on the PMU; otherwise a read() of the group.
 * Counts are user-space only (exclude_kernel), so they work at
 * perf_event_paranoid=2 and compare with `perf stat -e <event>:u`.
 *
 * Threads: the MPI_Init thread opens its group at init; threads created
 * afterwards open theirs at start via a pthread_create interposer; any
 * other thread opens lazily the first time it reaches a boundary.
 * Threads that existed before MPI_Init and never call MPI or a region
 * are not counted (OpenMP pools created before MPI_Init — phase P2's OMPT
 * hooks cover those).
 */

#include <stdint.h>

/* Semantic counter kinds: stable JSON keys, independent of vendor. */
typedef enum {
    CK_CYCLES = 0,
    CK_INSTRUCTIONS,
    CK_FLOPS,              /* FP operations, FMA counted as 2               */
    CK_L2_MISSES,          /* demand L2 misses (data + instruction)         */
    CK_DRAM_FILLS,         /* L1D fills sourced from DRAM/IO, any node      */
    CK_DRAM_FILLS_REMOTE,  /* ... from DRAM/IO on another NUMA node         */
    CK_FP_SCALAR,          /* retired FP ops by width                       */
    CK_FP_128,
    CK_FP_256,
    CK_FP_512,
    CK_COUNT
} cnt_kind_t;

#define CNT_MAX_EVENTS 6

typedef struct {
    cnt_kind_t  kind;
    const char *perf_name;   /* name `perf list` uses — for docs/validation */
    uint32_t    type;        /* PERF_TYPE_RAW or PERF_TYPE_HARDWARE         */
    uint64_t    config;
} cnt_event_t;

typedef struct {
    const char *uarch;       /* "zen4", "zen5", "generic"                   */
    const char *group;       /* "base", "vector"                            */
    int         n_required;  /* events [0, n_required) must schedule        */
    int         n_events;    /* events [n_required, n_events) are optional  */
    cnt_event_t ev[CNT_MAX_EVENTS];
} cnt_group_t;

/* AMD raw encoding: EventSelect[7:0] → config[7:0], UnitMask → [15:8],
 * EventSelect[11:8] → config[35:32] (sysfs: event=config:0-7,32-35). */
#define CNT_AMD_RAW(event, umask)                                    \
    ((uint64_t)((event) & 0xff) | ((uint64_t)((umask) & 0xff) << 8) | \
     ((uint64_t)((event) & 0xf00) << 24))

static inline uint64_t cnt_amd_raw(unsigned event, unsigned umask)
{
    return CNT_AMD_RAW(event, umask);
}

const char *cnt_kind_name(cnt_kind_t k);

/* Table lookup: uarch from sysinfo ("zen4", ...), group "base"/"vector".
 * Unknown uarch → the generic table (cycles + instructions). Never NULL. */
const cnt_group_t *cnt_lookup(const char *uarch, const char *group);

/* ── lifecycle (snapshot mode) ───────────────────────────────────────── */

/* Select the group and open it for the calling thread. Returns 1 if
 * counting, 0 if unavailable (reason via cnt_status()). Never fatal. */
int  cnt_init(const char *uarch, const char *group);
void cnt_stop(void);   /* stop boundary reads (start of finalize) */

typedef struct {
    int      available;
    char     reason[160];        /* why not, when !available                */
    const cnt_group_t *group;
    int      n_active;           /* events actually counting                */
    int      rdpmc;              /* 1 if the rdpmc fast path was enabled    */
    uint64_t reads_rdpmc;        /* boundary/region reads via rdpmc         */
    uint64_t reads_syscall;      /* ... via read() (fallback or disabled)   */
    int      threads;            /* thread groups opened on this rank       */
    int      threads_dropped;    /* threads beyond the slot table           */
    int      multiplexed;        /* some group was not on the PMU all along */
    uint64_t total  [CNT_MAX_EVENTS];   /* sum over threads, this rank      */
    uint64_t compute[CNT_MAX_EVENTS];
    uint64_t mpi    [CNT_MAX_EVENTS];
    uint64_t main_thread[CNT_MAX_EVENTS];  /* MPI_Init thread only         */
} cnt_result_t;

/* Final read of every thread on this rank; fills *out. */
void cnt_collect(cnt_result_t *out);

/* ── boundaries (hot path: called from MPI wrappers) ─────────────────── */
void cnt_mpi_enter(void);
void cnt_mpi_exit(void);

/* ── user regions ────────────────────────────────────────────────────── */
#define CNT_MAX_REGIONS   64
#define CNT_REGION_NAME   48
#define CNT_REGION_DEPTH  16

typedef struct {
    char     name[CNT_REGION_NAME];
    uint64_t calls;
    uint64_t ns;
    uint64_t ev[CNT_MAX_EVENTS];
} cnt_region_t;

/* Copy this rank's regions; returns the count. */
int cnt_regions_get(cnt_region_t *out, int max);

#endif
