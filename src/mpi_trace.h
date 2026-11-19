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
    MPIC_COMM,         /* communicator and topology creation/free: collective,
                          no user data, can take seconds at scale          */
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
    X(ACCUMULATE,           "MPI_Accumulate",           RMA)  \
    /* persistent point-to-point: bytes at MPI_Start, from the *_init */ \
    X(SEND_INIT,            "MPI_Send_init",            P2P)  \
    X(SSEND_INIT,           "MPI_Ssend_init",           P2P)  \
    X(RSEND_INIT,           "MPI_Rsend_init",           P2P)  \
    X(BSEND_INIT,           "MPI_Bsend_init",           P2P)  \
    X(RECV_INIT,            "MPI_Recv_init",            P2P)  \
    X(START,                "MPI_Start",                P2P)  \
    X(STARTALL,             "MPI_Startall",             P2P)  \
    /* matched probe */                                       \
    X(MPROBE,               "MPI_Mprobe",               WAIT) \
    X(IMPROBE,              "MPI_Improbe",              WAIT) \
    X(MRECV,                "MPI_Mrecv",                P2P)  \
    X(IMRECV,               "MPI_Imrecv",               P2P)  \
    /* nonblocking collectives: the post (time to Wait is WAIT) */ \
    X(IBARRIER,             "MPI_Ibarrier",             COLLECTIVE) \
    X(IBCAST,               "MPI_Ibcast",               COLLECTIVE) \
    X(IREDUCE,              "MPI_Ireduce",              COLLECTIVE) \
    X(IALLREDUCE,           "MPI_Iallreduce",           COLLECTIVE) \
    X(IGATHER,              "MPI_Igather",              COLLECTIVE) \
    X(IGATHERV,             "MPI_Igatherv",             COLLECTIVE) \
    X(IALLGATHER,           "MPI_Iallgather",           COLLECTIVE) \
    X(IALLGATHERV,          "MPI_Iallgatherv",          COLLECTIVE) \
    X(ISCATTER,             "MPI_Iscatter",             COLLECTIVE) \
    X(ISCATTERV,            "MPI_Iscatterv",            COLLECTIVE) \
    X(IALLTOALL,            "MPI_Ialltoall",            COLLECTIVE) \
    X(IALLTOALLV,           "MPI_Ialltoallv",           COLLECTIVE) \
    X(IREDUCE_SCATTER,      "MPI_Ireduce_scatter",      COLLECTIVE) \
    X(IREDUCE_SCATTER_BLOCK,"MPI_Ireduce_scatter_block",COLLECTIVE) \
    X(ISCAN,                "MPI_Iscan",                COLLECTIVE) \
    X(IEXSCAN,              "MPI_Iexscan",              COLLECTIVE) \
    /* neighbourhood collectives (topology communicators) */ \
    X(NEIGHBOR_ALLGATHER,   "MPI_Neighbor_allgather",   COLLECTIVE) \
    X(NEIGHBOR_ALLGATHERV,  "MPI_Neighbor_allgatherv",  COLLECTIVE) \
    X(NEIGHBOR_ALLTOALL,    "MPI_Neighbor_alltoall",    COLLECTIVE) \
    X(NEIGHBOR_ALLTOALLV,   "MPI_Neighbor_alltoallv",   COLLECTIVE) \
    X(INEIGHBOR_ALLGATHER,  "MPI_Ineighbor_allgather",  COLLECTIVE) \
    X(INEIGHBOR_ALLGATHERV, "MPI_Ineighbor_allgatherv", COLLECTIVE) \
    X(INEIGHBOR_ALLTOALL,   "MPI_Ineighbor_alltoall",   COLLECTIVE) \
    X(INEIGHBOR_ALLTOALLV,  "MPI_Ineighbor_alltoallv",  COLLECTIVE) \
    /* communicators and topologies */                        \
    X(COMM_DUP,             "MPI_Comm_dup",             COMM) \
    X(COMM_SPLIT,           "MPI_Comm_split",           COMM) \
    X(COMM_SPLIT_TYPE,      "MPI_Comm_split_type",      COMM) \
    X(COMM_CREATE,          "MPI_Comm_create",          COMM) \
    X(COMM_CREATE_GROUP,    "MPI_Comm_create_group",    COMM) \
    X(COMM_FREE,            "MPI_Comm_free",            COMM) \
    X(CART_CREATE,          "MPI_Cart_create",          COMM) \
    X(CART_SUB,             "MPI_Cart_sub",             COMM) \
    X(DIST_GRAPH_CREATE,    "MPI_Dist_graph_create",    COMM) \
    X(DIST_GRAPH_CREATE_ADJACENT, "MPI_Dist_graph_create_adjacent", COMM) \
    X(INTERCOMM_CREATE,     "MPI_Intercomm_create",     COMM) \
    X(INTERCOMM_MERGE,      "MPI_Intercomm_merge",      COMM) \
    /* one-sided: windows, passive target, PSCW, atomics, request-based */ \
    X(WIN_ALLOCATE,         "MPI_Win_allocate",         RMA)  \
    X(WIN_ALLOCATE_SHARED,  "MPI_Win_allocate_shared",  RMA)  \
    X(WIN_CREATE_DYNAMIC,   "MPI_Win_create_dynamic",   RMA)  \
    X(WIN_LOCK,             "MPI_Win_lock",             RMA)  \
    X(WIN_UNLOCK,           "MPI_Win_unlock",           RMA)  \
    X(WIN_LOCK_ALL,         "MPI_Win_lock_all",         RMA)  \
    X(WIN_UNLOCK_ALL,       "MPI_Win_unlock_all",       RMA)  \
    X(WIN_FLUSH,            "MPI_Win_flush",            RMA)  \
    X(WIN_FLUSH_ALL,        "MPI_Win_flush_all",        RMA)  \
    X(WIN_FLUSH_LOCAL,      "MPI_Win_flush_local",      RMA)  \
    X(WIN_FLUSH_LOCAL_ALL,  "MPI_Win_flush_local_all",  RMA)  \
    X(WIN_SYNC,             "MPI_Win_sync",             RMA)  \
    X(WIN_POST,             "MPI_Win_post",             RMA)  \
    X(WIN_START,            "MPI_Win_start",            RMA)  \
    X(WIN_COMPLETE,         "MPI_Win_complete",         RMA)  \
    X(WIN_WAIT,             "MPI_Win_wait",             RMA)  \
    X(GET_ACCUMULATE,       "MPI_Get_accumulate",       RMA)  \
    X(FETCH_AND_OP,         "MPI_Fetch_and_op",         RMA)  \
    X(COMPARE_AND_SWAP,     "MPI_Compare_and_swap",     RMA)  \
    X(RPUT,                 "MPI_Rput",                 RMA)  \
    X(RGET,                 "MPI_Rget",                 RMA)  \
    X(RACCUMULATE,          "MPI_Raccumulate",          RMA)

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
