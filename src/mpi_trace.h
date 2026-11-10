#ifndef MPI_TRACE_H
#define MPI_TRACE_H

/*
 * mpi_trace.h — PMPI interposition shared by trace mode and snapshot mode
 *
 * Every wrapped MPI function has a fixed ID (MPIF_*) and a class
 * (MPIC_*). Trace mode keeps its by-name table exactly as before; snapshot
 * mode accumulates per-ID totals and per-class message-size histograms
 * into fixed arrays that can be reduced across ranks without any name
 * matching.
 */

#include <stdint.h>

/* ── classes ─────────────────────────────────────────────────────────── */
typedef enum {
    MPIC_P2P = 0,      /* Send/Recv family, incl. nonblocking posts        */
    MPIC_COLLECTIVE,   /* blocking collectives                             */
    MPIC_WAIT,         /* Wait/Test/Probe family: completion and polling   */
    MPIC_RMA,          /* one-sided                                        */
    MPIC_COUNT
} mpi_class_t;

/* ── function table (X-macro: ID, name, class) ───────────────────────── */
#define MPI_TRACE_FUNCS(X)                                    \
    X(SEND,                 "MPI_Send",                 P2P)  \
    X(SSEND,                "MPI_Ssend",                P2P)  \
    X(RSEND,                "MPI_Rsend",                P2P)  \
    X(BSEND,                "MPI_Bsend",                P2P)  \
    X(RECV,                 "MPI_Recv",                 P2P)  \
    X(SENDRECV,             "MPI_Sendrecv",             P2P)  \
    X(SENDRECV_REPLACE,     "MPI_Sendrecv_replace",     P2P)  \
    X(ISEND,                "MPI_Isend",                P2P)  \
    X(ISSEND,               "MPI_Issend",               P2P)  \
    X(IRECV,                "MPI_Irecv",                P2P)  \
    X(WAIT,                 "MPI_Wait",                 WAIT) \
    X(WAITALL,              "MPI_Waitall",              WAIT) \
    X(WAITANY,              "MPI_Waitany",              WAIT) \
    X(WAITSOME,             "MPI_Waitsome",             WAIT) \
    X(TEST,                 "MPI_Test",                 WAIT) \
    X(TESTALL,              "MPI_Testall",              WAIT) \
    X(TESTANY,              "MPI_Testany",              WAIT) \
    X(TESTSOME,             "MPI_Testsome",             WAIT) \
    X(PROBE,                "MPI_Probe",                WAIT) \
    X(IPROBE,               "MPI_Iprobe",               WAIT) \
    X(BARRIER,              "MPI_Barrier",              COLLECTIVE) \
    X(BCAST,                "MPI_Bcast",                COLLECTIVE) \
    X(REDUCE,               "MPI_Reduce",               COLLECTIVE) \
    X(ALLREDUCE,            "MPI_Allreduce",            COLLECTIVE) \
    X(GATHER,               "MPI_Gather",               COLLECTIVE) \
    X(GATHERV,              "MPI_Gatherv",              COLLECTIVE) \
    X(ALLGATHER,            "MPI_Allgather",            COLLECTIVE) \
    X(ALLGATHERV,           "MPI_Allgatherv",           COLLECTIVE) \
    X(SCATTER,              "MPI_Scatter",              COLLECTIVE) \
    X(SCATTERV,             "MPI_Scatterv",             COLLECTIVE) \
    X(ALLTOALL,             "MPI_Alltoall",             COLLECTIVE) \
    X(ALLTOALLV,            "MPI_Alltoallv",            COLLECTIVE) \
    X(REDUCE_SCATTER,       "MPI_Reduce_scatter",       COLLECTIVE) \
    X(REDUCE_SCATTER_BLOCK, "MPI_Reduce_scatter_block", COLLECTIVE) \
    X(SCAN,                 "MPI_Scan",                 COLLECTIVE) \
    X(EXSCAN,               "MPI_Exscan",               COLLECTIVE) \
    X(WIN_CREATE,           "MPI_Win_create",           RMA)  \
    X(WIN_FREE,             "MPI_Win_free",             RMA)  \
    X(WIN_FENCE,            "MPI_Win_fence",            RMA)  \
    X(PUT,                  "MPI_Put",                  RMA)  \
    X(GET,                  "MPI_Get",                  RMA)  \
    X(ACCUMULATE,           "MPI_Accumulate",           RMA)

typedef enum {
#define X(id, name, cls) MPIF_##id,
    MPI_TRACE_FUNCS(X)
#undef X
    MPIF_COUNT
} mpi_func_id_t;

/* Message-size histogram: bin 0 = 0 bytes, bin b>0 = [2^(b-1), 2^b). */
#define MPI_HIST_BINS 34

/* Per-rank snapshot accumulators. Times in nanoseconds (integer, so they
 * can be accumulated atomically from several threads). */
typedef struct {
    uint64_t calls[MPIF_COUNT];
    uint64_t bytes[MPIF_COUNT];
    uint64_t ns[MPIF_COUNT];
    uint64_t hist_calls[MPIC_COUNT][MPI_HIST_BINS];
    uint64_t hist_ns   [MPIC_COUNT][MPI_HIST_BINS];
} mpi_snap_t;

/* ── control ─────────────────────────────────────────────────────────── */
#define MPI_TRACE_OFF      0
#define MPI_TRACE_TRACE    1   /* trace mode: by-name table + trace_store bridge */
#define MPI_TRACE_SNAPSHOT 2   /* snapshot mode: per-ID accumulators            */

void mpi_trace_set_mode(int mode);
void mpi_trace_set_enabled(int enabled);   /* compat: 1 → TRACE, 0 → OFF */

const char *mpi_trace_func_name (int id);
int         mpi_trace_func_class(int id);
const char *mpi_trace_class_name(int cls);  /* "p2p", "collective", ... */

/* trace mode: by-name table (unchanged interface) */
void mpi_trace_iterate(void (*cb)(const char *name, long calls,
                                  double total_s, long bytes, void *arg),
                       void *arg);

/* snapshot mode: copy this rank's accumulators */
void mpi_trace_snapshot_get(mpi_snap_t *out);

/* ── tool-depth guard ────────────────────────────────────────────────── *
 * Non-zero while the calling thread is inside a wrapped MPI or MPI-IO call.
 * Wrappers seeing it set pass straight through without recording, so
 *   - a Fortran wrapper → library Fortran binding → C MPI_* (as some MPI
 *     libraries implement it) is counted once, not twice, and
 *   - POSIX I/O done by the MPI library itself (ROMIO/OMPIO, shared-memory
 *     transports) is not counted as application I/O.
 */
extern _Thread_local int g_miniprof_tool_depth;

#endif
