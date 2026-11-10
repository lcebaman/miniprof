/*
 * mpi_trace.c — PMPI interposition for trace mode and snapshot mode
 *
 * Every wrapped MPI call here, when a recording mode is active:
 *   1. Records entry time
 *   2. Calls through to the PMPI binding
 *   3. Records elapsed time + message bytes, attributed to a fixed
 *      function ID (see MPI_TRACE_FUNCS in mpi_trace.h)
 *
 * Trace mode feeds a by-name table (Table 2 of the trace report) and the
 * trace_store MPI-time bridge used for exclusive-time accounting, exactly
 * as before. Snapshot mode feeds fixed per-ID / per-class arrays that are
 * reduced across ranks at finalize. With no recording mode active every
 * wrapper is a straight call to PMPI_* (one branch of overhead).
 *
 * ── Fortran bindings ─────────────────────────────────────────────────────
 * Fortran's `mpi` module calls separately-mangled symbols (mpi_send_,
 * mpi_allreduce_, ...) that bypass the C wrappers entirely. The Fortran
 * wrappers below time the call and forward to the MPI library's OWN
 * Fortran PMPI binding (pmpi_send_, ...), never to the C API.
 *
 * An earlier version converted the arguments to C (MPI_*_f2c) and called
 * the C wrappers. That silently broke every Fortran sentinel: Fortran's
 * MPI_IN_PLACE, MPI_BOTTOM and MPI_STATUS_IGNORE are addresses of
 * library-private common-block variables, not the C constants, so the C
 * call treated MPI_IN_PLACE as a real (zero-filled) send buffer —
 * confirmed: MPI_Allreduce(MPI_IN_PLACE, x, ...) returned all zeros under
 * miniprof in every mode, and the correct sum without it. Forwarding to the
 * library's Fortran binding leaves all sentinel handling to the library,
 * which is the only component that knows its own sentinel addresses.
 * `make run-fortran-sentinels` is the regression test.
 *
 * Byte counts never read an argument the standard says may be ignored:
 * with MPI_IN_PLACE the send count/datatype of gather/allgather/alltoall
 * are undefined (an invalid datatype handle can crash PMPI_Type_size), so
 * those use the receive side, or the side that is valid on this rank.
 * Bytes are therefore "this rank's share of the data", documented per
 * call below. For receives it is the posted (maximum) size.
 *
 * In snapshot mode each recorded call is also a counter boundary
 * (cnt_mpi_enter/exit, counters.c): counts between calls are attributed to
 * "compute", counts inside to "mpi". The reads sit outside the timed
 * window, so they never inflate measured MPI time.
 *
 * ── Nesting guard ────────────────────────────────────────────────────────
 * g_miniprof_tool_depth (thread-local) is raised for the duration of each
 * recorded call; any wrapper entered while it is raised passes straight
 * through. So a library whose Fortran binding calls the C MPI_* symbol is
 * counted once, and the I/O wrappers in io_trace.c ignore POSIX I/O done
 * by the MPI library itself (MPI-IO back ends, shared-memory transports).
 *
 * Not wrapped (fall through with zero overhead, counted as compute time):
 * nonblocking collectives (their completion time IS counted, in
 * Wait/Test), passive-target RMA, communicator/group management, and the
 * mpi_f08 module's bindings (mpi_send_f08, ...).
 */

#include "mpi_trace.h"
#include "trace_store.h"
#include "counters.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

_Thread_local int g_miniprof_tool_depth = 0;

/* ── function/class tables ───────────────────────────────────────────── */
static const char *k_func_names[MPIF_COUNT] = {
#define X(id, name, cls) name,
    MPI_TRACE_FUNCS(X)
#undef X
};
static const unsigned char k_func_class[MPIF_COUNT] = {
#define X(id, name, cls) MPIC_##cls,
    MPI_TRACE_FUNCS(X)
#undef X
};
static const char *k_class_names[MPIC_COUNT] = {
    "p2p", "collective", "wait", "rma"
};

const char *mpi_trace_func_name(int id)
{
    return (id >= 0 && id < MPIF_COUNT) ? k_func_names[id] : "?";
}
int mpi_trace_func_class(int id)
{
    return (id >= 0 && id < MPIF_COUNT) ? k_func_class[id] : -1;
}
const char *mpi_trace_class_name(int cls)
{
    return (cls >= 0 && cls < MPIC_COUNT) ? k_class_names[cls] : "?";
}

/* ── trace mode: small by-name table (unchanged behaviour) ───────────── */
typedef struct {
    char   name[MINIPROF_SYMBOL_LEN];
    _Atomic(long)   calls;
    double total_time_s;   /* single-threaded-per-rank assumption, see note
                              in trace_store.c about non-atomic doubles    */
    long   total_bytes;
} mpi_trace_slot_t;

#define MPI_TRACE_MAX_FUNCS 64
static mpi_trace_slot_t g_mpi_table[MPI_TRACE_MAX_FUNCS];
static int               g_mpi_n_used = 0;

/* ── snapshot mode accumulators ──────────────────────────────────────── */
static mpi_snap_t g_snap;

static int g_mode = MPI_TRACE_OFF;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void mpi_trace_set_mode(int mode)
{
    g_mode = mode;
    if (mode == MPI_TRACE_TRACE) {
        memset(g_mpi_table, 0, sizeof(g_mpi_table));
        g_mpi_n_used = 0;
    } else if (mode == MPI_TRACE_SNAPSHOT) {
        memset(&g_snap, 0, sizeof(g_snap));
    }
}

void mpi_trace_set_enabled(int enabled)
{
    mpi_trace_set_mode(enabled ? MPI_TRACE_TRACE : MPI_TRACE_OFF);
}

/* find-or-create slot by name (linear scan — table is tiny) */
static mpi_trace_slot_t *get_slot(const char *name)
{
    for (int i = 0; i < g_mpi_n_used; i++)
        if (strcmp(g_mpi_table[i].name, name) == 0) return &g_mpi_table[i];

    if (g_mpi_n_used >= MPI_TRACE_MAX_FUNCS) return NULL; /* table full */
    mpi_trace_slot_t *s = &g_mpi_table[g_mpi_n_used++];
    snprintf(s->name, MINIPROF_SYMBOL_LEN, "%s", name);
    return s;
}

static inline int hist_bin(long bytes)
{
    if (bytes <= 0) return 0;
    int b = 64 - __builtin_clzl((unsigned long)bytes);   /* 1 → 1, 2..3 → 2, ... */
    return b < MPI_HIST_BINS ? b : MPI_HIST_BINS - 1;
}

static void record(int id, uint64_t dt_ns, long bytes)
{
    if (g_mode == MPI_TRACE_SNAPSHOT) {
        int cls = k_func_class[id], bin = hist_bin(bytes);
        __atomic_fetch_add(&g_snap.calls[id], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_snap.bytes[id], (uint64_t)(bytes > 0 ? bytes : 0),
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_snap.ns[id], dt_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_snap.hist_calls[cls][bin], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_snap.hist_ns[cls][bin], dt_ns, __ATOMIC_RELAXED);
        return;
    }

    /* trace mode */
    double elapsed = dt_ns * 1e-9;
    mpi_trace_slot_t *s = get_slot(k_func_names[id]);
    if (s) {
        atomic_fetch_add_explicit(&s->calls, 1L, memory_order_relaxed);
        s->total_time_s += elapsed;
        s->total_bytes   += bytes;
    }
    /* Feed this call's duration into trace_store's running MPI-time
     * counter, so any traced user function currently on the call stack
     * correctly excludes this time from its own "exclusive" total. */
    trace_store_add_mpi_time(elapsed);
}

void mpi_trace_iterate(void (*cb)(const char *name, long calls,
                                  double total_s, long bytes, void *arg),
                       void *arg)
{
    for (int i = 0; i < g_mpi_n_used; i++) {
        long calls = atomic_load(&g_mpi_table[i].calls);
        if (calls > 0)
            cb(g_mpi_table[i].name, calls,
               g_mpi_table[i].total_time_s, g_mpi_table[i].total_bytes, arg);
    }
}

void mpi_trace_snapshot_get(mpi_snap_t *out)
{
    /* relaxed atomic loads, element by element */
    uint64_t *dst = (uint64_t *)out;
    const uint64_t *src = (const uint64_t *)&g_snap;
    for (size_t i = 0; i < sizeof(g_snap) / sizeof(uint64_t); i++)
        dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

/* ── byte helpers ────────────────────────────────────────────────────── */

static inline long type_size(MPI_Datatype dt)
{
    if (dt == MPI_DATATYPE_NULL) return 0;
    int s = 0;
    if (PMPI_Type_size(dt, &s) != MPI_SUCCESS || s < 0) return 0;
    return s;
}
static inline long nbytes(long count, MPI_Datatype dt)
{
    return count > 0 ? count * type_size(dt) : 0;
}
static inline int comm_rank(MPI_Comm c) { int r = -1; PMPI_Comm_rank(c, &r); return r; }
static inline int comm_size(MPI_Comm c) { int n = 0;  PMPI_Comm_size(c, &n); return n; }
static inline long sum_counts(const int *counts, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) if (counts[i] > 0) s += counts[i];
    return s;
}

/* Fortran-handle versions */
static inline MPI_Datatype fdt(const MPI_Fint *d) { return MPI_Type_f2c(*d); }
static inline MPI_Comm     fcm(const MPI_Fint *c) { return MPI_Comm_f2c(*c); }
static inline long fbytes(const MPI_Fint *count, const MPI_Fint *dt)
{
    return nbytes((long)*count, fdt(dt));
}
static inline long fsum_counts(const MPI_Fint *counts, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) if (counts[i] > 0) s += counts[i];
    return s;
}

/* ── wrapper macros ──────────────────────────────────────────────────── *
 * CALL is the full PMPI call expression; BYTES is evaluated after it, so
 * byte accounting (which may itself call PMPI_Type_size/Comm_rank) is
 * not counted as MPI time. */
#define C_WRAP(ID, CALL, BYTES)                                    \
    do {                                                           \
        if (!g_mode || g_miniprof_tool_depth) return CALL;         \
        g_miniprof_tool_depth++;                                   \
        if (g_mode == MPI_TRACE_SNAPSHOT) cnt_mpi_enter();         \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = CALL;                                            \
        uint64_t dt_ = now_ns() - t0_;                             \
        if (g_mode == MPI_TRACE_SNAPSHOT) cnt_mpi_exit();          \
        g_miniprof_tool_depth--;                                   \
        record(MPIF_##ID, dt_, (BYTES));                           \
        return rc_;                                                \
    } while (0)

#define F_WRAP(ID, CALL, BYTES)                                    \
    do {                                                           \
        if (!g_mode || g_miniprof_tool_depth) { CALL; return; }    \
        g_miniprof_tool_depth++;                                   \
        if (g_mode == MPI_TRACE_SNAPSHOT) cnt_mpi_enter();         \
        uint64_t t0_ = now_ns();                                   \
        CALL;                                                      \
        uint64_t dt_ = now_ns() - t0_;                             \
        if (g_mode == MPI_TRACE_SNAPSHOT) cnt_mpi_exit();          \
        g_miniprof_tool_depth--;                                   \
        record(MPIF_##ID, dt_, (BYTES));                           \
    } while (0)

/* ════════════════════════════════════════════════════════════════════════
 *  C bindings
 * ════════════════════════════════════════════════════════════════════════ */

/* ── point-to-point: bytes = this call's send/posted-receive size ────── */
int MPI_Send(const void *buf, int count, MPI_Datatype dt, int dest,
             int tag, MPI_Comm comm)
{ C_WRAP(SEND, PMPI_Send(buf, count, dt, dest, tag, comm), nbytes(count, dt)); }

int MPI_Ssend(const void *buf, int count, MPI_Datatype dt, int dest,
              int tag, MPI_Comm comm)
{ C_WRAP(SSEND, PMPI_Ssend(buf, count, dt, dest, tag, comm), nbytes(count, dt)); }

int MPI_Rsend(const void *buf, int count, MPI_Datatype dt, int dest,
              int tag, MPI_Comm comm)
{ C_WRAP(RSEND, PMPI_Rsend(buf, count, dt, dest, tag, comm), nbytes(count, dt)); }

int MPI_Bsend(const void *buf, int count, MPI_Datatype dt, int dest,
              int tag, MPI_Comm comm)
{ C_WRAP(BSEND, PMPI_Bsend(buf, count, dt, dest, tag, comm), nbytes(count, dt)); }

int MPI_Recv(void *buf, int count, MPI_Datatype dt, int source,
             int tag, MPI_Comm comm, MPI_Status *status)
{ C_WRAP(RECV, PMPI_Recv(buf, count, dt, source, tag, comm, status), nbytes(count, dt)); }

int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int source, int recvtag, MPI_Comm comm, MPI_Status *status)
{
    C_WRAP(SENDRECV,
           PMPI_Sendrecv(sendbuf, sendcount, sendtype, dest, sendtag,
                         recvbuf, recvcount, recvtype, source, recvtag,
                         comm, status),
           nbytes(sendcount, sendtype));
}

int MPI_Sendrecv_replace(void *buf, int count, MPI_Datatype dt,
                         int dest, int sendtag, int source, int recvtag,
                         MPI_Comm comm, MPI_Status *status)
{
    C_WRAP(SENDRECV_REPLACE,
           PMPI_Sendrecv_replace(buf, count, dt, dest, sendtag, source,
                                 recvtag, comm, status),
           nbytes(count, dt));
}

int MPI_Isend(const void *buf, int count, MPI_Datatype dt, int dest,
              int tag, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(ISEND, PMPI_Isend(buf, count, dt, dest, tag, comm, req), nbytes(count, dt)); }

int MPI_Issend(const void *buf, int count, MPI_Datatype dt, int dest,
               int tag, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(ISSEND, PMPI_Issend(buf, count, dt, dest, tag, comm, req), nbytes(count, dt)); }

int MPI_Irecv(void *buf, int count, MPI_Datatype dt, int source,
              int tag, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(IRECV, PMPI_Irecv(buf, count, dt, source, tag, comm, req), nbytes(count, dt)); }

/* ── completion / polling: no bytes ──────────────────────────────────── */
int MPI_Wait(MPI_Request *req, MPI_Status *status)
{ C_WRAP(WAIT, PMPI_Wait(req, status), 0); }

int MPI_Waitall(int count, MPI_Request reqs[], MPI_Status stats[])
{ C_WRAP(WAITALL, PMPI_Waitall(count, reqs, stats), 0); }

int MPI_Waitany(int count, MPI_Request reqs[], int *index, MPI_Status *status)
{ C_WRAP(WAITANY, PMPI_Waitany(count, reqs, index, status), 0); }

int MPI_Waitsome(int incount, MPI_Request reqs[], int *outcount,
                 int indices[], MPI_Status stats[])
{ C_WRAP(WAITSOME, PMPI_Waitsome(incount, reqs, outcount, indices, stats), 0); }

int MPI_Test(MPI_Request *req, int *flag, MPI_Status *status)
{ C_WRAP(TEST, PMPI_Test(req, flag, status), 0); }

int MPI_Testall(int count, MPI_Request reqs[], int *flag, MPI_Status stats[])
{ C_WRAP(TESTALL, PMPI_Testall(count, reqs, flag, stats), 0); }

int MPI_Testany(int count, MPI_Request reqs[], int *index, int *flag,
                MPI_Status *status)
{ C_WRAP(TESTANY, PMPI_Testany(count, reqs, index, flag, status), 0); }

int MPI_Testsome(int incount, MPI_Request reqs[], int *outcount,
                 int indices[], MPI_Status stats[])
{ C_WRAP(TESTSOME, PMPI_Testsome(incount, reqs, outcount, indices, stats), 0); }

int MPI_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status)
{ C_WRAP(PROBE, PMPI_Probe(source, tag, comm, status), 0); }

int MPI_Iprobe(int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status)
{ C_WRAP(IPROBE, PMPI_Iprobe(source, tag, comm, flag, status), 0); }

/* ── collectives: bytes = this rank's share, never from an argument that
 *    MPI_IN_PLACE makes undefined ───────────────────────────────────── */
int MPI_Barrier(MPI_Comm comm)
{ C_WRAP(BARRIER, PMPI_Barrier(comm), 0); }

int MPI_Bcast(void *buf, int count, MPI_Datatype dt, int root, MPI_Comm comm)
{ C_WRAP(BCAST, PMPI_Bcast(buf, count, dt, root, comm), nbytes(count, dt)); }

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype dt, MPI_Op op, int root, MPI_Comm comm)
{ C_WRAP(REDUCE, PMPI_Reduce(sendbuf, recvbuf, count, dt, op, root, comm), nbytes(count, dt)); }

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype dt, MPI_Op op, MPI_Comm comm)
{ C_WRAP(ALLREDUCE, PMPI_Allreduce(sendbuf, recvbuf, count, dt, op, comm), nbytes(count, dt)); }

/* root: its recv slot (send args may be IN_PLACE); others: send side */
int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm)
{
    C_WRAP(GATHER,
           PMPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, root, comm),
           comm_rank(comm) == root ? nbytes(recvcount, recvtype)
                                   : nbytes(sendcount, sendtype));
}

int MPI_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, const int recvcounts[], const int displs[],
                MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    C_WRAP(GATHERV,
           PMPI_Gatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                        displs, recvtype, root, comm),
           comm_rank(comm) == root ? nbytes(recvcounts[root], recvtype)
                                   : nbytes(sendcount, sendtype));
}

/* recv side: valid on every rank, with or without IN_PLACE */
int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm)
{
    C_WRAP(ALLGATHER,
           PMPI_Allgather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                          recvtype, comm),
           nbytes(recvcount, recvtype));
}

int MPI_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, const int recvcounts[], const int displs[],
                   MPI_Datatype recvtype, MPI_Comm comm)
{
    C_WRAP(ALLGATHERV,
           PMPI_Allgatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                           displs, recvtype, comm),
           nbytes(recvcounts[comm_rank(comm)], recvtype));
}

/* root: its own send slot (recv args may be IN_PLACE); others: recv side */
int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm)
{
    C_WRAP(SCATTER,
           PMPI_Scatter(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                        recvtype, root, comm),
           comm_rank(comm) == root ? nbytes(sendcount, sendtype)
                                   : nbytes(recvcount, recvtype));
}

int MPI_Scatterv(const void *sendbuf, const int sendcounts[], const int displs[],
                 MPI_Datatype sendtype, void *recvbuf, int recvcount,
                 MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    C_WRAP(SCATTERV,
           PMPI_Scatterv(sendbuf, sendcounts, displs, sendtype, recvbuf,
                         recvcount, recvtype, root, comm),
           comm_rank(comm) == root ? nbytes(sendcounts[root], sendtype)
                                   : nbytes(recvcount, recvtype));
}

/* recv side: total this rank receives */
int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm)
{
    C_WRAP(ALLTOALL,
           PMPI_Alltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                         recvtype, comm),
           nbytes((long)recvcount * comm_size(comm), recvtype));
}

int MPI_Alltoallv(const void *sendbuf, const int sendcounts[],
                  const int sdispls[], MPI_Datatype sendtype,
                  void *recvbuf, const int recvcounts[], const int rdispls[],
                  MPI_Datatype recvtype, MPI_Comm comm)
{
    C_WRAP(ALLTOALLV,
           PMPI_Alltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                          recvcounts, rdispls, recvtype, comm),
           nbytes(sum_counts(recvcounts, comm_size(comm)), recvtype));
}

/* input vector length (count/datatype stay valid with IN_PLACE) */
int MPI_Reduce_scatter(const void *sendbuf, void *recvbuf,
                       const int recvcounts[], MPI_Datatype dt, MPI_Op op,
                       MPI_Comm comm)
{
    C_WRAP(REDUCE_SCATTER,
           PMPI_Reduce_scatter(sendbuf, recvbuf, recvcounts, dt, op, comm),
           nbytes(sum_counts(recvcounts, comm_size(comm)), dt));
}

int MPI_Reduce_scatter_block(const void *sendbuf, void *recvbuf, int recvcount,
                             MPI_Datatype dt, MPI_Op op, MPI_Comm comm)
{
    C_WRAP(REDUCE_SCATTER_BLOCK,
           PMPI_Reduce_scatter_block(sendbuf, recvbuf, recvcount, dt, op, comm),
           nbytes((long)recvcount * comm_size(comm), dt));
}

int MPI_Scan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype dt,
             MPI_Op op, MPI_Comm comm)
{ C_WRAP(SCAN, PMPI_Scan(sendbuf, recvbuf, count, dt, op, comm), nbytes(count, dt)); }

int MPI_Exscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype dt,
               MPI_Op op, MPI_Comm comm)
{ C_WRAP(EXSCAN, PMPI_Exscan(sendbuf, recvbuf, count, dt, op, comm), nbytes(count, dt)); }

/* ── one-sided (active target): bytes = origin buffer size ───────────── */
int MPI_Win_create(void *base, MPI_Aint size, int disp_unit, MPI_Info info,
                   MPI_Comm comm, MPI_Win *win)
{ C_WRAP(WIN_CREATE, PMPI_Win_create(base, size, disp_unit, info, comm, win), 0); }

int MPI_Win_free(MPI_Win *win)
{ C_WRAP(WIN_FREE, PMPI_Win_free(win), 0); }

int MPI_Win_fence(int assert, MPI_Win win)
{ C_WRAP(WIN_FENCE, PMPI_Win_fence(assert, win), 0); }

int MPI_Put(const void *origin_addr, int origin_count, MPI_Datatype origin_dt,
            int target_rank, MPI_Aint target_disp, int target_count,
            MPI_Datatype target_dt, MPI_Win win)
{
    C_WRAP(PUT,
           PMPI_Put(origin_addr, origin_count, origin_dt, target_rank,
                    target_disp, target_count, target_dt, win),
           nbytes(origin_count, origin_dt));
}

int MPI_Get(void *origin_addr, int origin_count, MPI_Datatype origin_dt,
            int target_rank, MPI_Aint target_disp, int target_count,
            MPI_Datatype target_dt, MPI_Win win)
{
    C_WRAP(GET,
           PMPI_Get(origin_addr, origin_count, origin_dt, target_rank,
                    target_disp, target_count, target_dt, win),
           nbytes(origin_count, origin_dt));
}

int MPI_Accumulate(const void *origin_addr, int origin_count,
                   MPI_Datatype origin_dt, int target_rank,
                   MPI_Aint target_disp, int target_count,
                   MPI_Datatype target_dt, MPI_Op op, MPI_Win win)
{
    C_WRAP(ACCUMULATE,
           PMPI_Accumulate(origin_addr, origin_count, origin_dt, target_rank,
                           target_disp, target_count, target_dt, op, win),
           nbytes(origin_count, origin_dt));
}

/* ════════════════════════════════════════════════════════════════════════
 *  Fortran bindings (`use mpi` / mpif.h, trailing-underscore mangling)
 *
 *  Forward to the library's Fortran PMPI binding — see file header. All
 *  arguments are passed through untouched, so sentinels (MPI_IN_PLACE,
 *  MPI_BOTTOM, MPI_STATUS(ES)_IGNORE) are interpreted by the library.
 *  The pmpi_*_ symbols live in the library's Fortran shim (libmpi_mpifh
 *  for Open MPI), linked by mpifort; like pmpi_init_ in
 *  miniprof_fortran.c they resolve lazily, so C-only programs are fine.
 * ════════════════════════════════════════════════════════════════════════ */

typedef MPI_Fint F;   /* brevity: every Fortran INTEGER argument */

extern void pmpi_send_(const void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ssend_(const void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_rsend_(const void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_bsend_(const void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_recv_(void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_sendrecv_(const void*, F*, F*, F*, F*, void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_sendrecv_replace_(void*, F*, F*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_isend_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_issend_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_irecv_(void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_wait_(F*, F*, F*);
extern void pmpi_waitall_(F*, F*, F*, F*);
extern void pmpi_waitany_(F*, F*, F*, F*, F*);
extern void pmpi_waitsome_(F*, F*, F*, F*, F*, F*);
extern void pmpi_test_(F*, F*, F*, F*);
extern void pmpi_testall_(F*, F*, F*, F*, F*);
extern void pmpi_testany_(F*, F*, F*, F*, F*, F*);
extern void pmpi_testsome_(F*, F*, F*, F*, F*, F*);
extern void pmpi_probe_(F*, F*, F*, F*, F*);
extern void pmpi_iprobe_(F*, F*, F*, F*, F*, F*);
extern void pmpi_barrier_(F*, F*);
extern void pmpi_bcast_(void*, F*, F*, F*, F*, F*);
extern void pmpi_reduce_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_allreduce_(const void*, void*, F*, F*, F*, F*, F*);
extern void pmpi_gather_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_gatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_allgather_(const void*, F*, F*, void*, F*, F*, F*, F*);
extern void pmpi_allgatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_scatter_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_scatterv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_alltoall_(const void*, F*, F*, void*, F*, F*, F*, F*);
extern void pmpi_alltoallv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_reduce_scatter_(const void*, void*, F*, F*, F*, F*, F*);
extern void pmpi_reduce_scatter_block_(const void*, void*, F*, F*, F*, F*, F*);
extern void pmpi_scan_(const void*, void*, F*, F*, F*, F*, F*);
extern void pmpi_exscan_(const void*, void*, F*, F*, F*, F*, F*);
extern void pmpi_win_create_(void*, MPI_Aint*, F*, F*, F*, F*, F*);
extern void pmpi_win_free_(F*, F*);
extern void pmpi_win_fence_(F*, F*, F*);
extern void pmpi_put_(const void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*);
extern void pmpi_get_(void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*);
extern void pmpi_accumulate_(const void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*, F*);

/* ── point-to-point ──────────────────────────────────────────────────── */
void mpi_send_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *ierr)
{ F_WRAP(SEND, pmpi_send_(buf, count, dt, dest, tag, comm, ierr), fbytes(count, dt)); }

void mpi_ssend_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *ierr)
{ F_WRAP(SSEND, pmpi_ssend_(buf, count, dt, dest, tag, comm, ierr), fbytes(count, dt)); }

void mpi_rsend_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *ierr)
{ F_WRAP(RSEND, pmpi_rsend_(buf, count, dt, dest, tag, comm, ierr), fbytes(count, dt)); }

void mpi_bsend_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *ierr)
{ F_WRAP(BSEND, pmpi_bsend_(buf, count, dt, dest, tag, comm, ierr), fbytes(count, dt)); }

void mpi_recv_(void *buf, F *count, F *dt, F *source, F *tag, F *comm,
               F *status, F *ierr)
{ F_WRAP(RECV, pmpi_recv_(buf, count, dt, source, tag, comm, status, ierr), fbytes(count, dt)); }

void mpi_sendrecv_(const void *sendbuf, F *sendcount, F *sendtype, F *dest, F *sendtag,
                   void *recvbuf, F *recvcount, F *recvtype, F *source, F *recvtag,
                   F *comm, F *status, F *ierr)
{
    F_WRAP(SENDRECV,
           pmpi_sendrecv_(sendbuf, sendcount, sendtype, dest, sendtag,
                          recvbuf, recvcount, recvtype, source, recvtag,
                          comm, status, ierr),
           fbytes(sendcount, sendtype));
}

void mpi_sendrecv_replace_(void *buf, F *count, F *dt, F *dest, F *sendtag,
                           F *source, F *recvtag, F *comm, F *status, F *ierr)
{
    F_WRAP(SENDRECV_REPLACE,
           pmpi_sendrecv_replace_(buf, count, dt, dest, sendtag, source,
                                  recvtag, comm, status, ierr),
           fbytes(count, dt));
}

void mpi_isend_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm,
                F *req, F *ierr)
{ F_WRAP(ISEND, pmpi_isend_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt)); }

void mpi_issend_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm,
                 F *req, F *ierr)
{ F_WRAP(ISSEND, pmpi_issend_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt)); }

void mpi_irecv_(void *buf, F *count, F *dt, F *source, F *tag, F *comm,
                F *req, F *ierr)
{ F_WRAP(IRECV, pmpi_irecv_(buf, count, dt, source, tag, comm, req, ierr), fbytes(count, dt)); }

/* ── completion / polling ────────────────────────────────────────────── */
void mpi_wait_(F *req, F *status, F *ierr)
{ F_WRAP(WAIT, pmpi_wait_(req, status, ierr), 0); }

void mpi_waitall_(F *count, F *reqs, F *stats, F *ierr)
{ F_WRAP(WAITALL, pmpi_waitall_(count, reqs, stats, ierr), 0); }

void mpi_waitany_(F *count, F *reqs, F *index, F *status, F *ierr)
{ F_WRAP(WAITANY, pmpi_waitany_(count, reqs, index, status, ierr), 0); }

void mpi_waitsome_(F *incount, F *reqs, F *outcount, F *indices, F *stats, F *ierr)
{ F_WRAP(WAITSOME, pmpi_waitsome_(incount, reqs, outcount, indices, stats, ierr), 0); }

void mpi_test_(F *req, F *flag, F *status, F *ierr)
{ F_WRAP(TEST, pmpi_test_(req, flag, status, ierr), 0); }

void mpi_testall_(F *count, F *reqs, F *flag, F *stats, F *ierr)
{ F_WRAP(TESTALL, pmpi_testall_(count, reqs, flag, stats, ierr), 0); }

void mpi_testany_(F *count, F *reqs, F *index, F *flag, F *status, F *ierr)
{ F_WRAP(TESTANY, pmpi_testany_(count, reqs, index, flag, status, ierr), 0); }

void mpi_testsome_(F *incount, F *reqs, F *outcount, F *indices, F *stats, F *ierr)
{ F_WRAP(TESTSOME, pmpi_testsome_(incount, reqs, outcount, indices, stats, ierr), 0); }

void mpi_probe_(F *source, F *tag, F *comm, F *status, F *ierr)
{ F_WRAP(PROBE, pmpi_probe_(source, tag, comm, status, ierr), 0); }

void mpi_iprobe_(F *source, F *tag, F *comm, F *flag, F *status, F *ierr)
{ F_WRAP(IPROBE, pmpi_iprobe_(source, tag, comm, flag, status, ierr), 0); }

/* ── collectives (same byte rules as the C bindings) ─────────────────── */
void mpi_barrier_(F *comm, F *ierr)
{ F_WRAP(BARRIER, pmpi_barrier_(comm, ierr), 0); }

void mpi_bcast_(void *buf, F *count, F *dt, F *root, F *comm, F *ierr)
{ F_WRAP(BCAST, pmpi_bcast_(buf, count, dt, root, comm, ierr), fbytes(count, dt)); }

void mpi_reduce_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                 F *root, F *comm, F *ierr)
{ F_WRAP(REDUCE, pmpi_reduce_(sendbuf, recvbuf, count, dt, op, root, comm, ierr), fbytes(count, dt)); }

void mpi_allreduce_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                    F *comm, F *ierr)
{ F_WRAP(ALLREDUCE, pmpi_allreduce_(sendbuf, recvbuf, count, dt, op, comm, ierr), fbytes(count, dt)); }

void mpi_gather_(const void *sendbuf, F *sendcount, F *sendtype,
                 void *recvbuf, F *recvcount, F *recvtype, F *root, F *comm, F *ierr)
{
    F_WRAP(GATHER,
           pmpi_gather_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                        recvtype, root, comm, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(recvcount, recvtype)
                                         : fbytes(sendcount, sendtype));
}

void mpi_gatherv_(const void *sendbuf, F *sendcount, F *sendtype,
                  void *recvbuf, F *recvcounts, F *displs, F *recvtype,
                  F *root, F *comm, F *ierr)
{
    F_WRAP(GATHERV,
           pmpi_gatherv_(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                         displs, recvtype, root, comm, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(&recvcounts[*root], recvtype)
                                         : fbytes(sendcount, sendtype));
}

void mpi_allgather_(const void *sendbuf, F *sendcount, F *sendtype,
                    void *recvbuf, F *recvcount, F *recvtype, F *comm, F *ierr)
{
    F_WRAP(ALLGATHER,
           pmpi_allgather_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                           recvtype, comm, ierr),
           fbytes(recvcount, recvtype));
}

void mpi_allgatherv_(const void *sendbuf, F *sendcount, F *sendtype,
                     void *recvbuf, F *recvcounts, F *displs, F *recvtype,
                     F *comm, F *ierr)
{
    F_WRAP(ALLGATHERV,
           pmpi_allgatherv_(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                            displs, recvtype, comm, ierr),
           fbytes(&recvcounts[comm_rank(fcm(comm))], recvtype));
}

void mpi_scatter_(const void *sendbuf, F *sendcount, F *sendtype,
                  void *recvbuf, F *recvcount, F *recvtype, F *root, F *comm, F *ierr)
{
    F_WRAP(SCATTER,
           pmpi_scatter_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                         recvtype, root, comm, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(sendcount, sendtype)
                                         : fbytes(recvcount, recvtype));
}

void mpi_scatterv_(const void *sendbuf, F *sendcounts, F *displs, F *sendtype,
                   void *recvbuf, F *recvcount, F *recvtype, F *root, F *comm, F *ierr)
{
    F_WRAP(SCATTERV,
           pmpi_scatterv_(sendbuf, sendcounts, displs, sendtype, recvbuf,
                          recvcount, recvtype, root, comm, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(&sendcounts[*root], sendtype)
                                         : fbytes(recvcount, recvtype));
}

void mpi_alltoall_(const void *sendbuf, F *sendcount, F *sendtype,
                   void *recvbuf, F *recvcount, F *recvtype, F *comm, F *ierr)
{
    F_WRAP(ALLTOALL,
           pmpi_alltoall_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                          recvtype, comm, ierr),
           nbytes((long)*recvcount * comm_size(fcm(comm)), fdt(recvtype)));
}

void mpi_alltoallv_(const void *sendbuf, F *sendcounts, F *sdispls, F *sendtype,
                    void *recvbuf, F *recvcounts, F *rdispls, F *recvtype,
                    F *comm, F *ierr)
{
    F_WRAP(ALLTOALLV,
           pmpi_alltoallv_(sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                           recvcounts, rdispls, recvtype, comm, ierr),
           nbytes(fsum_counts(recvcounts, comm_size(fcm(comm))), fdt(recvtype)));
}

void mpi_reduce_scatter_(const void *sendbuf, void *recvbuf, F *recvcounts,
                         F *dt, F *op, F *comm, F *ierr)
{
    F_WRAP(REDUCE_SCATTER,
           pmpi_reduce_scatter_(sendbuf, recvbuf, recvcounts, dt, op, comm, ierr),
           nbytes(fsum_counts(recvcounts, comm_size(fcm(comm))), fdt(dt)));
}

void mpi_reduce_scatter_block_(const void *sendbuf, void *recvbuf, F *recvcount,
                               F *dt, F *op, F *comm, F *ierr)
{
    F_WRAP(REDUCE_SCATTER_BLOCK,
           pmpi_reduce_scatter_block_(sendbuf, recvbuf, recvcount, dt, op, comm, ierr),
           nbytes((long)*recvcount * comm_size(fcm(comm)), fdt(dt)));
}

void mpi_scan_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
               F *comm, F *ierr)
{ F_WRAP(SCAN, pmpi_scan_(sendbuf, recvbuf, count, dt, op, comm, ierr), fbytes(count, dt)); }

void mpi_exscan_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                 F *comm, F *ierr)
{ F_WRAP(EXSCAN, pmpi_exscan_(sendbuf, recvbuf, count, dt, op, comm, ierr), fbytes(count, dt)); }

/* ── one-sided. MPI_Aint arguments are INTEGER(KIND=MPI_ADDRESS_KIND) in
 *    Fortran: address-sized, passed by reference like everything else. ── */
void mpi_win_create_(void *base, MPI_Aint *size, F *disp_unit, F *info,
                     F *comm, F *win, F *ierr)
{ F_WRAP(WIN_CREATE, pmpi_win_create_(base, size, disp_unit, info, comm, win, ierr), 0); }

void mpi_win_free_(F *win, F *ierr)
{ F_WRAP(WIN_FREE, pmpi_win_free_(win, ierr), 0); }

void mpi_win_fence_(F *assert, F *win, F *ierr)
{ F_WRAP(WIN_FENCE, pmpi_win_fence_(assert, win, ierr), 0); }

void mpi_put_(const void *origin, F *ocount, F *odt, F *target_rank,
              MPI_Aint *target_disp, F *tcount, F *tdt, F *win, F *ierr)
{
    F_WRAP(PUT,
           pmpi_put_(origin, ocount, odt, target_rank, target_disp, tcount,
                     tdt, win, ierr),
           fbytes(ocount, odt));
}

void mpi_get_(void *origin, F *ocount, F *odt, F *target_rank,
              MPI_Aint *target_disp, F *tcount, F *tdt, F *win, F *ierr)
{
    F_WRAP(GET,
           pmpi_get_(origin, ocount, odt, target_rank, target_disp, tcount,
                     tdt, win, ierr),
           fbytes(ocount, odt));
}

void mpi_accumulate_(const void *origin, F *ocount, F *odt, F *target_rank,
                     MPI_Aint *target_disp, F *tcount, F *tdt, F *op,
                     F *win, F *ierr)
{
    F_WRAP(ACCUMULATE,
           pmpi_accumulate_(origin, ocount, odt, target_rank, target_disp,
                            tcount, tdt, op, win, ierr),
           fbytes(ocount, odt));
}
