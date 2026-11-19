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
#include "timeline.h"
#include "trace_store.h"
#include "counters.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
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
    "p2p", "collective", "wait", "rma", "comm"
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

#define MPI_TRACE_MAX_FUNCS MPIF_COUNT   /* every wrapped function fits */
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
        if (tl_on) tl_add(TL_MPI, now_ns(), dt_ns);
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
 *  Coverage added after the audit: persistent requests, matched probe,
 *  nonblocking and neighbourhood collectives, communicators, more RMA.
 * ════════════════════════════════════════════════════════════════════════ */

/* ── persistent requests ─────────────────────────────────────────────── *
 * A persistent send or receive is described once (*_init) and started
 * many times. Its bytes belong to each MPI_Start, so *_init records the
 * request's size here and MPI_Start/Startall look it up. Open addressing,
 * keyed by the request handle (a pointer in Open MPI, an int in MPICH).
 * Inserts and removals (rare: *_init, MPI_Request_free) take a lock;
 * lookups (every MPI_Start) are lock-free. A full table only loses bytes. */
#define PREQ_SLOTS 4096                       /* power of two */
#define PREQ_EMPTY 0u
#define PREQ_GONE  ((uintptr_t)-1)
static uintptr_t g_preq_key[PREQ_SLOTS];
static long      g_preq_bytes[PREQ_SLOTS];
static pthread_mutex_t g_preq_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uintptr_t req_key(MPI_Request r)
{
    uintptr_t k = 0;
    memcpy(&k, &r, sizeof(r) < sizeof(k) ? sizeof(r) : sizeof(k));
    return k == PREQ_EMPTY || k == PREQ_GONE ? 1 : k;
}
static inline unsigned preq_hash(uintptr_t k)
{
    k ^= k >> 17; k *= 0xed5ad4bbu; k ^= k >> 11;
    return (unsigned)k & (PREQ_SLOTS - 1);
}
static void preq_put(MPI_Request r, long bytes)
{
    if (r == MPI_REQUEST_NULL) return;
    uintptr_t k = req_key(r);
    pthread_mutex_lock(&g_preq_lock);
    unsigned h = preq_hash(k), tomb = PREQ_SLOTS;
    for (unsigned i = 0; i < PREQ_SLOTS; i++, h = (h + 1) & (PREQ_SLOTS - 1)) {
        uintptr_t cur = __atomic_load_n(&g_preq_key[h], __ATOMIC_RELAXED);
        if (cur == k) { __atomic_store_n(&g_preq_bytes[h], bytes, __ATOMIC_RELAXED); break; }
        if (cur == PREQ_GONE && tomb == PREQ_SLOTS) tomb = h;
        if (cur == PREQ_EMPTY) {
            unsigned at = tomb != PREQ_SLOTS ? tomb : h;
            __atomic_store_n(&g_preq_bytes[at], bytes, __ATOMIC_RELAXED);
            __atomic_store_n(&g_preq_key[at], k, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&g_preq_lock);
}
static long preq_get(MPI_Request r)
{
    if (r == MPI_REQUEST_NULL) return 0;
    uintptr_t k = req_key(r);
    unsigned h = preq_hash(k);
    for (unsigned i = 0; i < PREQ_SLOTS; i++, h = (h + 1) & (PREQ_SLOTS - 1)) {
        uintptr_t cur = __atomic_load_n(&g_preq_key[h], __ATOMIC_ACQUIRE);
        if (cur == k) return __atomic_load_n(&g_preq_bytes[h], __ATOMIC_RELAXED);
        if (cur == PREQ_EMPTY) return 0;
    }
    return 0;
}
static void preq_del(MPI_Request r)
{
    if (r == MPI_REQUEST_NULL) return;
    uintptr_t k = req_key(r);
    pthread_mutex_lock(&g_preq_lock);
    unsigned h = preq_hash(k);
    for (unsigned i = 0; i < PREQ_SLOTS; i++, h = (h + 1) & (PREQ_SLOTS - 1)) {
        uintptr_t cur = __atomic_load_n(&g_preq_key[h], __ATOMIC_RELAXED);
        if (cur == k) { __atomic_store_n(&g_preq_key[h], PREQ_GONE, __ATOMIC_RELEASE); break; }
        if (cur == PREQ_EMPTY) break;
    }
    pthread_mutex_unlock(&g_preq_lock);
}
static long preq_sum(int n, const MPI_Request *r)
{
    long s = 0;
    for (int i = 0; i < n; i++) s += preq_get(r[i]);
    return s;
}

/* *_init: record the call (no bytes: nothing moves yet), remember size */
#define PINIT_WRAP(ID, CALL, BYTES, REQ)                           \
    do {                                                           \
        if (!g_mode || g_miniprof_tool_depth) return CALL;         \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = CALL;                                            \
        uint64_t dt_ = now_ns() - t0_;                             \
        g_miniprof_tool_depth--;                                   \
        if (rc_ == MPI_SUCCESS) preq_put(*(REQ), (BYTES));         \
        record(MPIF_##ID, dt_, 0);                                 \
        return rc_;                                                \
    } while (0)

int MPI_Send_init(const void *buf, int count, MPI_Datatype dt, int dest,
                  int tag, MPI_Comm comm, MPI_Request *req)
{ PINIT_WRAP(SEND_INIT, PMPI_Send_init(buf, count, dt, dest, tag, comm, req), nbytes(count, dt), req); }

int MPI_Ssend_init(const void *buf, int count, MPI_Datatype dt, int dest,
                   int tag, MPI_Comm comm, MPI_Request *req)
{ PINIT_WRAP(SSEND_INIT, PMPI_Ssend_init(buf, count, dt, dest, tag, comm, req), nbytes(count, dt), req); }

int MPI_Rsend_init(const void *buf, int count, MPI_Datatype dt, int dest,
                   int tag, MPI_Comm comm, MPI_Request *req)
{ PINIT_WRAP(RSEND_INIT, PMPI_Rsend_init(buf, count, dt, dest, tag, comm, req), nbytes(count, dt), req); }

int MPI_Bsend_init(const void *buf, int count, MPI_Datatype dt, int dest,
                   int tag, MPI_Comm comm, MPI_Request *req)
{ PINIT_WRAP(BSEND_INIT, PMPI_Bsend_init(buf, count, dt, dest, tag, comm, req), nbytes(count, dt), req); }

int MPI_Recv_init(void *buf, int count, MPI_Datatype dt, int source,
                  int tag, MPI_Comm comm, MPI_Request *req)
{ PINIT_WRAP(RECV_INIT, PMPI_Recv_init(buf, count, dt, source, tag, comm, req), nbytes(count, dt), req); }

/* bytes looked up before the call: MPI_Start does not change the handle */
int MPI_Start(MPI_Request *req)
{
    long b = (g_mode && !g_miniprof_tool_depth) ? preq_get(*req) : 0;
    C_WRAP(START, PMPI_Start(req), b);
}

int MPI_Startall(int count, MPI_Request reqs[])
{
    long b = (g_mode && !g_miniprof_tool_depth) ? preq_sum(count, reqs) : 0;
    C_WRAP(STARTALL, PMPI_Startall(count, reqs), b);
}

/* not timed: local bookkeeping; forgets a persistent request's size */
int MPI_Request_free(MPI_Request *req)
{
    if (req) preq_del(*req);
    return PMPI_Request_free(req);
}

/* ── matched probe ───────────────────────────────────────────────────── */
int MPI_Mprobe(int source, int tag, MPI_Comm comm, MPI_Message *msg,
               MPI_Status *status)
{ C_WRAP(MPROBE, PMPI_Mprobe(source, tag, comm, msg, status), 0); }

int MPI_Improbe(int source, int tag, MPI_Comm comm, int *flag,
                MPI_Message *msg, MPI_Status *status)
{ C_WRAP(IMPROBE, PMPI_Improbe(source, tag, comm, flag, msg, status), 0); }

int MPI_Mrecv(void *buf, int count, MPI_Datatype dt, MPI_Message *msg,
              MPI_Status *status)
{ C_WRAP(MRECV, PMPI_Mrecv(buf, count, dt, msg, status), nbytes(count, dt)); }

int MPI_Imrecv(void *buf, int count, MPI_Datatype dt, MPI_Message *msg,
               MPI_Request *req)
{ C_WRAP(IMRECV, PMPI_Imrecv(buf, count, dt, msg, req), nbytes(count, dt)); }

/* ── nonblocking collectives: same byte rules as the blocking forms ──── */
int MPI_Ibarrier(MPI_Comm comm, MPI_Request *req)
{ C_WRAP(IBARRIER, PMPI_Ibarrier(comm, req), 0); }

int MPI_Ibcast(void *buf, int count, MPI_Datatype dt, int root, MPI_Comm comm,
               MPI_Request *req)
{ C_WRAP(IBCAST, PMPI_Ibcast(buf, count, dt, root, comm, req), nbytes(count, dt)); }

int MPI_Ireduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype dt,
                MPI_Op op, int root, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(IREDUCE, PMPI_Ireduce(sendbuf, recvbuf, count, dt, op, root, comm, req), nbytes(count, dt)); }

int MPI_Iallreduce(const void *sendbuf, void *recvbuf, int count,
                   MPI_Datatype dt, MPI_Op op, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(IALLREDUCE, PMPI_Iallreduce(sendbuf, recvbuf, count, dt, op, comm, req), nbytes(count, dt)); }

int MPI_Igather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IGATHER,
           PMPI_Igather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                        recvtype, root, comm, req),
           comm_rank(comm) == root ? nbytes(recvcount, recvtype)
                                   : nbytes(sendcount, sendtype));
}

int MPI_Igatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, const int recvcounts[], const int displs[],
                 MPI_Datatype recvtype, int root, MPI_Comm comm,
                 MPI_Request *req)
{
    C_WRAP(IGATHERV,
           PMPI_Igatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                         displs, recvtype, root, comm, req),
           comm_rank(comm) == root ? nbytes(recvcounts[root], recvtype)
                                   : nbytes(sendcount, sendtype));
}

int MPI_Iallgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                   MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IALLGATHER,
           PMPI_Iallgather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                           recvtype, comm, req),
           nbytes(recvcount, recvtype));
}

int MPI_Iallgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, const int recvcounts[], const int displs[],
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IALLGATHERV,
           PMPI_Iallgatherv(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                            displs, recvtype, comm, req),
           nbytes(recvcounts[comm_rank(comm)], recvtype));
}

int MPI_Iscatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int root, MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(ISCATTER,
           PMPI_Iscatter(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                         recvtype, root, comm, req),
           comm_rank(comm) == root ? nbytes(sendcount, sendtype)
                                   : nbytes(recvcount, recvtype));
}

int MPI_Iscatterv(const void *sendbuf, const int sendcounts[], const int displs[],
                  MPI_Datatype sendtype, void *recvbuf, int recvcount,
                  MPI_Datatype recvtype, int root, MPI_Comm comm,
                  MPI_Request *req)
{
    C_WRAP(ISCATTERV,
           PMPI_Iscatterv(sendbuf, sendcounts, displs, sendtype, recvbuf,
                          recvcount, recvtype, root, comm, req),
           comm_rank(comm) == root ? nbytes(sendcounts[root], sendtype)
                                   : nbytes(recvcount, recvtype));
}

int MPI_Ialltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IALLTOALL,
           PMPI_Ialltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                          recvtype, comm, req),
           nbytes((long)recvcount * comm_size(comm), recvtype));
}

int MPI_Ialltoallv(const void *sendbuf, const int sendcounts[],
                   const int sdispls[], MPI_Datatype sendtype,
                   void *recvbuf, const int recvcounts[], const int rdispls[],
                   MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IALLTOALLV,
           PMPI_Ialltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                           recvcounts, rdispls, recvtype, comm, req),
           nbytes(sum_counts(recvcounts, comm_size(comm)), recvtype));
}

int MPI_Ireduce_scatter(const void *sendbuf, void *recvbuf,
                        const int recvcounts[], MPI_Datatype dt, MPI_Op op,
                        MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(IREDUCE_SCATTER,
           PMPI_Ireduce_scatter(sendbuf, recvbuf, recvcounts, dt, op, comm, req),
           nbytes(sum_counts(recvcounts, comm_size(comm)), dt));
}

int MPI_Ireduce_scatter_block(const void *sendbuf, void *recvbuf, int recvcount,
                              MPI_Datatype dt, MPI_Op op, MPI_Comm comm,
                              MPI_Request *req)
{
    C_WRAP(IREDUCE_SCATTER_BLOCK,
           PMPI_Ireduce_scatter_block(sendbuf, recvbuf, recvcount, dt, op, comm, req),
           nbytes((long)recvcount * comm_size(comm), dt));
}

int MPI_Iscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype dt,
              MPI_Op op, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(ISCAN, PMPI_Iscan(sendbuf, recvbuf, count, dt, op, comm, req), nbytes(count, dt)); }

int MPI_Iexscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype dt,
                MPI_Op op, MPI_Comm comm, MPI_Request *req)
{ C_WRAP(IEXSCAN, PMPI_Iexscan(sendbuf, recvbuf, count, dt, op, comm, req), nbytes(count, dt)); }

/* ── neighbourhood collectives: recv side, over this rank's in-neighbours
 *    (MPI_IN_PLACE is not allowed for them) ───────────────────────────── */
static int in_degree(MPI_Comm comm)
{
    int topo = MPI_UNDEFINED;
    if (PMPI_Topo_test(comm, &topo) != MPI_SUCCESS) return 0;
    if (topo == MPI_CART) {
        int nd = 0;
        PMPI_Cartdim_get(comm, &nd);
        return 2 * nd;
    }
    if (topo == MPI_GRAPH) {
        int n = 0;
        PMPI_Graph_neighbors_count(comm, comm_rank(comm), &n);
        return n;
    }
    if (topo == MPI_DIST_GRAPH) {
        int in = 0, out = 0, w = 0;
        PMPI_Dist_graph_neighbors_count(comm, &in, &out, &w);
        return in;
    }
    return 0;
}

int MPI_Neighbor_allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                           void *recvbuf, int recvcount, MPI_Datatype recvtype,
                           MPI_Comm comm)
{
    C_WRAP(NEIGHBOR_ALLGATHER,
           PMPI_Neighbor_allgather(sendbuf, sendcount, sendtype, recvbuf,
                                   recvcount, recvtype, comm),
           nbytes((long)recvcount * in_degree(comm), recvtype));
}

int MPI_Neighbor_allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                            void *recvbuf, const int recvcounts[], const int displs[],
                            MPI_Datatype recvtype, MPI_Comm comm)
{
    C_WRAP(NEIGHBOR_ALLGATHERV,
           PMPI_Neighbor_allgatherv(sendbuf, sendcount, sendtype, recvbuf,
                                    recvcounts, displs, recvtype, comm),
           nbytes(sum_counts(recvcounts, in_degree(comm)), recvtype));
}

int MPI_Neighbor_alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                          void *recvbuf, int recvcount, MPI_Datatype recvtype,
                          MPI_Comm comm)
{
    C_WRAP(NEIGHBOR_ALLTOALL,
           PMPI_Neighbor_alltoall(sendbuf, sendcount, sendtype, recvbuf,
                                  recvcount, recvtype, comm),
           nbytes((long)recvcount * in_degree(comm), recvtype));
}

int MPI_Neighbor_alltoallv(const void *sendbuf, const int sendcounts[],
                           const int sdispls[], MPI_Datatype sendtype,
                           void *recvbuf, const int recvcounts[],
                           const int rdispls[], MPI_Datatype recvtype,
                           MPI_Comm comm)
{
    C_WRAP(NEIGHBOR_ALLTOALLV,
           PMPI_Neighbor_alltoallv(sendbuf, sendcounts, sdispls, sendtype,
                                   recvbuf, recvcounts, rdispls, recvtype, comm),
           nbytes(sum_counts(recvcounts, in_degree(comm)), recvtype));
}

int MPI_Ineighbor_allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                            void *recvbuf, int recvcount, MPI_Datatype recvtype,
                            MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(INEIGHBOR_ALLGATHER,
           PMPI_Ineighbor_allgather(sendbuf, sendcount, sendtype, recvbuf,
                                    recvcount, recvtype, comm, req),
           nbytes((long)recvcount * in_degree(comm), recvtype));
}

int MPI_Ineighbor_allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                             void *recvbuf, const int recvcounts[], const int displs[],
                             MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(INEIGHBOR_ALLGATHERV,
           PMPI_Ineighbor_allgatherv(sendbuf, sendcount, sendtype, recvbuf,
                                     recvcounts, displs, recvtype, comm, req),
           nbytes(sum_counts(recvcounts, in_degree(comm)), recvtype));
}

int MPI_Ineighbor_alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                           void *recvbuf, int recvcount, MPI_Datatype recvtype,
                           MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(INEIGHBOR_ALLTOALL,
           PMPI_Ineighbor_alltoall(sendbuf, sendcount, sendtype, recvbuf,
                                   recvcount, recvtype, comm, req),
           nbytes((long)recvcount * in_degree(comm), recvtype));
}

int MPI_Ineighbor_alltoallv(const void *sendbuf, const int sendcounts[],
                            const int sdispls[], MPI_Datatype sendtype,
                            void *recvbuf, const int recvcounts[],
                            const int rdispls[], MPI_Datatype recvtype,
                            MPI_Comm comm, MPI_Request *req)
{
    C_WRAP(INEIGHBOR_ALLTOALLV,
           PMPI_Ineighbor_alltoallv(sendbuf, sendcounts, sdispls, sendtype,
                                    recvbuf, recvcounts, rdispls, recvtype,
                                    comm, req),
           nbytes(sum_counts(recvcounts, in_degree(comm)), recvtype));
}

/* ── communicators and topologies: time only ─────────────────────────── */
int MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm)
{ C_WRAP(COMM_DUP, PMPI_Comm_dup(comm, newcomm), 0); }

int MPI_Comm_split(MPI_Comm comm, int color, int key, MPI_Comm *newcomm)
{ C_WRAP(COMM_SPLIT, PMPI_Comm_split(comm, color, key, newcomm), 0); }

int MPI_Comm_split_type(MPI_Comm comm, int split_type, int key, MPI_Info info,
                        MPI_Comm *newcomm)
{ C_WRAP(COMM_SPLIT_TYPE, PMPI_Comm_split_type(comm, split_type, key, info, newcomm), 0); }

int MPI_Comm_create(MPI_Comm comm, MPI_Group group, MPI_Comm *newcomm)
{ C_WRAP(COMM_CREATE, PMPI_Comm_create(comm, group, newcomm), 0); }

int MPI_Comm_create_group(MPI_Comm comm, MPI_Group group, int tag,
                          MPI_Comm *newcomm)
{ C_WRAP(COMM_CREATE_GROUP, PMPI_Comm_create_group(comm, group, tag, newcomm), 0); }

int MPI_Comm_free(MPI_Comm *comm)
{ C_WRAP(COMM_FREE, PMPI_Comm_free(comm), 0); }

int MPI_Cart_create(MPI_Comm comm, int ndims, const int dims[],
                    const int periods[], int reorder, MPI_Comm *newcomm)
{ C_WRAP(CART_CREATE, PMPI_Cart_create(comm, ndims, dims, periods, reorder, newcomm), 0); }

int MPI_Cart_sub(MPI_Comm comm, const int remain_dims[], MPI_Comm *newcomm)
{ C_WRAP(CART_SUB, PMPI_Cart_sub(comm, remain_dims, newcomm), 0); }

int MPI_Dist_graph_create(MPI_Comm comm, int n, const int sources[],
                          const int degrees[], const int destinations[],
                          const int weights[], MPI_Info info, int reorder,
                          MPI_Comm *newcomm)
{
    C_WRAP(DIST_GRAPH_CREATE,
           PMPI_Dist_graph_create(comm, n, sources, degrees, destinations,
                                  weights, info, reorder, newcomm), 0);
}

int MPI_Dist_graph_create_adjacent(MPI_Comm comm, int indegree,
                                   const int sources[], const int sourceweights[],
                                   int outdegree, const int destinations[],
                                   const int destweights[], MPI_Info info,
                                   int reorder, MPI_Comm *newcomm)
{
    C_WRAP(DIST_GRAPH_CREATE_ADJACENT,
           PMPI_Dist_graph_create_adjacent(comm, indegree, sources, sourceweights,
                                           outdegree, destinations, destweights,
                                           info, reorder, newcomm), 0);
}

int MPI_Intercomm_create(MPI_Comm local_comm, int local_leader,
                         MPI_Comm peer_comm, int remote_leader, int tag,
                         MPI_Comm *newintercomm)
{
    C_WRAP(INTERCOMM_CREATE,
           PMPI_Intercomm_create(local_comm, local_leader, peer_comm,
                                 remote_leader, tag, newintercomm), 0);
}

int MPI_Intercomm_merge(MPI_Comm intercomm, int high, MPI_Comm *newintracomm)
{ C_WRAP(INTERCOMM_MERGE, PMPI_Intercomm_merge(intercomm, high, newintracomm), 0); }

/* ── one-sided: windows, synchronisation, atomics, request-based ─────── */
int MPI_Win_allocate(MPI_Aint size, int disp_unit, MPI_Info info,
                     MPI_Comm comm, void *baseptr, MPI_Win *win)
{ C_WRAP(WIN_ALLOCATE, PMPI_Win_allocate(size, disp_unit, info, comm, baseptr, win), 0); }

int MPI_Win_allocate_shared(MPI_Aint size, int disp_unit, MPI_Info info,
                            MPI_Comm comm, void *baseptr, MPI_Win *win)
{ C_WRAP(WIN_ALLOCATE_SHARED, PMPI_Win_allocate_shared(size, disp_unit, info, comm, baseptr, win), 0); }

int MPI_Win_create_dynamic(MPI_Info info, MPI_Comm comm, MPI_Win *win)
{ C_WRAP(WIN_CREATE_DYNAMIC, PMPI_Win_create_dynamic(info, comm, win), 0); }

int MPI_Win_lock(int lock_type, int rank, int assert, MPI_Win win)
{ C_WRAP(WIN_LOCK, PMPI_Win_lock(lock_type, rank, assert, win), 0); }

int MPI_Win_unlock(int rank, MPI_Win win)
{ C_WRAP(WIN_UNLOCK, PMPI_Win_unlock(rank, win), 0); }

int MPI_Win_lock_all(int assert, MPI_Win win)
{ C_WRAP(WIN_LOCK_ALL, PMPI_Win_lock_all(assert, win), 0); }

int MPI_Win_unlock_all(MPI_Win win)
{ C_WRAP(WIN_UNLOCK_ALL, PMPI_Win_unlock_all(win), 0); }

int MPI_Win_flush(int rank, MPI_Win win)
{ C_WRAP(WIN_FLUSH, PMPI_Win_flush(rank, win), 0); }

int MPI_Win_flush_all(MPI_Win win)
{ C_WRAP(WIN_FLUSH_ALL, PMPI_Win_flush_all(win), 0); }

int MPI_Win_flush_local(int rank, MPI_Win win)
{ C_WRAP(WIN_FLUSH_LOCAL, PMPI_Win_flush_local(rank, win), 0); }

int MPI_Win_flush_local_all(MPI_Win win)
{ C_WRAP(WIN_FLUSH_LOCAL_ALL, PMPI_Win_flush_local_all(win), 0); }

int MPI_Win_sync(MPI_Win win)
{ C_WRAP(WIN_SYNC, PMPI_Win_sync(win), 0); }

int MPI_Win_post(MPI_Group group, int assert, MPI_Win win)
{ C_WRAP(WIN_POST, PMPI_Win_post(group, assert, win), 0); }

int MPI_Win_start(MPI_Group group, int assert, MPI_Win win)
{ C_WRAP(WIN_START, PMPI_Win_start(group, assert, win), 0); }

int MPI_Win_complete(MPI_Win win)
{ C_WRAP(WIN_COMPLETE, PMPI_Win_complete(win), 0); }

int MPI_Win_wait(MPI_Win win)
{ C_WRAP(WIN_WAIT, PMPI_Win_wait(win), 0); }

/* result side: always valid (the origin is ignored with MPI_NO_OP) */
int MPI_Get_accumulate(const void *origin_addr, int origin_count,
                       MPI_Datatype origin_dt, void *result_addr,
                       int result_count, MPI_Datatype result_dt,
                       int target_rank, MPI_Aint target_disp, int target_count,
                       MPI_Datatype target_dt, MPI_Op op, MPI_Win win)
{
    C_WRAP(GET_ACCUMULATE,
           PMPI_Get_accumulate(origin_addr, origin_count, origin_dt,
                               result_addr, result_count, result_dt,
                               target_rank, target_disp, target_count,
                               target_dt, op, win),
           nbytes(result_count, result_dt));
}

int MPI_Fetch_and_op(const void *origin_addr, void *result_addr,
                     MPI_Datatype dt, int target_rank, MPI_Aint target_disp,
                     MPI_Op op, MPI_Win win)
{
    C_WRAP(FETCH_AND_OP,
           PMPI_Fetch_and_op(origin_addr, result_addr, dt, target_rank,
                             target_disp, op, win),
           nbytes(1, dt));
}

int MPI_Compare_and_swap(const void *origin_addr, const void *compare_addr,
                         void *result_addr, MPI_Datatype dt, int target_rank,
                         MPI_Aint target_disp, MPI_Win win)
{
    C_WRAP(COMPARE_AND_SWAP,
           PMPI_Compare_and_swap(origin_addr, compare_addr, result_addr, dt,
                                 target_rank, target_disp, win),
           nbytes(1, dt));
}

int MPI_Rput(const void *origin_addr, int origin_count, MPI_Datatype origin_dt,
             int target_rank, MPI_Aint target_disp, int target_count,
             MPI_Datatype target_dt, MPI_Win win, MPI_Request *req)
{
    C_WRAP(RPUT,
           PMPI_Rput(origin_addr, origin_count, origin_dt, target_rank,
                     target_disp, target_count, target_dt, win, req),
           nbytes(origin_count, origin_dt));
}

int MPI_Rget(void *origin_addr, int origin_count, MPI_Datatype origin_dt,
             int target_rank, MPI_Aint target_disp, int target_count,
             MPI_Datatype target_dt, MPI_Win win, MPI_Request *req)
{
    C_WRAP(RGET,
           PMPI_Rget(origin_addr, origin_count, origin_dt, target_rank,
                     target_disp, target_count, target_dt, win, req),
           nbytes(origin_count, origin_dt));
}

int MPI_Raccumulate(const void *origin_addr, int origin_count,
                    MPI_Datatype origin_dt, int target_rank,
                    MPI_Aint target_disp, int target_count,
                    MPI_Datatype target_dt, MPI_Op op, MPI_Win win,
                    MPI_Request *req)
{
    C_WRAP(RACCUMULATE,
           PMPI_Raccumulate(origin_addr, origin_count, origin_dt, target_rank,
                            target_disp, target_count, target_dt, op, win, req),
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

/* ── coverage added after the audit (see the C section) ──────────────── */
extern void pmpi_send_init_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ssend_init_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_rsend_init_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_bsend_init_(const void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_recv_init_(void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_start_(F*, F*);
extern void pmpi_startall_(F*, F*, F*);
extern void pmpi_request_free_(F*, F*);
extern void pmpi_mprobe_(F*, F*, F*, F*, F*, F*);
extern void pmpi_improbe_(F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_mrecv_(void*, F*, F*, F*, F*, F*);
extern void pmpi_imrecv_(void*, F*, F*, F*, F*, F*);
extern void pmpi_ibarrier_(F*, F*, F*);
extern void pmpi_ibcast_(void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ireduce_(const void*, void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iallreduce_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_igather_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_igatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iallgather_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_iallgatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iscatter_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iscatterv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ialltoall_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_ialltoallv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ireduce_scatter_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ireduce_scatter_block_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iscan_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_iexscan_(const void*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_neighbor_allgather_(const void*, F*, F*, void*, F*, F*, F*, F*);
extern void pmpi_neighbor_allgatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_neighbor_alltoall_(const void*, F*, F*, void*, F*, F*, F*, F*);
extern void pmpi_neighbor_alltoallv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_ineighbor_allgather_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_ineighbor_allgatherv_(const void*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_ineighbor_alltoall_(const void*, F*, F*, void*, F*, F*, F*, F*, F*);
extern void pmpi_ineighbor_alltoallv_(const void*, F*, F*, F*, void*, F*, F*, F*, F*, F*, F*);
extern void pmpi_comm_dup_(F*, F*, F*);
extern void pmpi_comm_split_(F*, F*, F*, F*, F*);
extern void pmpi_comm_split_type_(F*, F*, F*, F*, F*, F*);
extern void pmpi_comm_create_(F*, F*, F*, F*);
extern void pmpi_comm_create_group_(F*, F*, F*, F*, F*);
extern void pmpi_comm_free_(F*, F*);
extern void pmpi_cart_create_(F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_cart_sub_(F*, F*, F*, F*);
extern void pmpi_dist_graph_create_(F*, F*, F*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_dist_graph_create_adjacent_(F*, F*, F*, F*, F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_intercomm_create_(F*, F*, F*, F*, F*, F*, F*);
extern void pmpi_intercomm_merge_(F*, F*, F*, F*);
extern void pmpi_win_allocate_(MPI_Aint*, F*, F*, F*, MPI_Aint*, F*, F*);
extern void pmpi_win_allocate_shared_(MPI_Aint*, F*, F*, F*, MPI_Aint*, F*, F*);
extern void pmpi_win_create_dynamic_(F*, F*, F*, F*);
extern void pmpi_win_lock_(F*, F*, F*, F*, F*);
extern void pmpi_win_unlock_(F*, F*, F*);
extern void pmpi_win_lock_all_(F*, F*, F*);
extern void pmpi_win_unlock_all_(F*, F*);
extern void pmpi_win_flush_(F*, F*, F*);
extern void pmpi_win_flush_all_(F*, F*);
extern void pmpi_win_flush_local_(F*, F*, F*);
extern void pmpi_win_flush_local_all_(F*, F*);
extern void pmpi_win_sync_(F*, F*);
extern void pmpi_win_post_(F*, F*, F*, F*);
extern void pmpi_win_start_(F*, F*, F*, F*);
extern void pmpi_win_complete_(F*, F*);
extern void pmpi_win_wait_(F*, F*);
extern void pmpi_get_accumulate_(const void*, F*, F*, void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*, F*);
extern void pmpi_fetch_and_op_(const void*, void*, F*, F*, MPI_Aint*, F*, F*, F*);
extern void pmpi_compare_and_swap_(const void*, const void*, void*, F*, F*, MPI_Aint*, F*, F*);
extern void pmpi_rput_(const void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*, F*);
extern void pmpi_rget_(void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*, F*);
extern void pmpi_raccumulate_(const void*, F*, F*, F*, MPI_Aint*, F*, F*, F*, F*, F*, F*);

static inline MPI_Request freq(const F *r) { return MPI_Request_f2c(*r); }

/* persistent: same as C, request looked up through its C handle */
#define FPINIT_WRAP(ID, CALL, BYTES, REQ)                          \
    do {                                                           \
        if (!g_mode || g_miniprof_tool_depth) { CALL; return; }    \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        CALL;                                                      \
        uint64_t dt_ = now_ns() - t0_;                             \
        g_miniprof_tool_depth--;                                   \
        if (*ierr == MPI_SUCCESS) preq_put(freq(REQ), (BYTES));    \
        record(MPIF_##ID, dt_, 0);                                 \
    } while (0)

void mpi_send_init_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *req, F *ierr)
{ FPINIT_WRAP(SEND_INIT, pmpi_send_init_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt), req); }

void mpi_ssend_init_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *req, F *ierr)
{ FPINIT_WRAP(SSEND_INIT, pmpi_ssend_init_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt), req); }

void mpi_rsend_init_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *req, F *ierr)
{ FPINIT_WRAP(RSEND_INIT, pmpi_rsend_init_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt), req); }

void mpi_bsend_init_(const void *buf, F *count, F *dt, F *dest, F *tag, F *comm, F *req, F *ierr)
{ FPINIT_WRAP(BSEND_INIT, pmpi_bsend_init_(buf, count, dt, dest, tag, comm, req, ierr), fbytes(count, dt), req); }

void mpi_recv_init_(void *buf, F *count, F *dt, F *source, F *tag, F *comm, F *req, F *ierr)
{ FPINIT_WRAP(RECV_INIT, pmpi_recv_init_(buf, count, dt, source, tag, comm, req, ierr), fbytes(count, dt), req); }

void mpi_start_(F *req, F *ierr)
{
    long b = (g_mode && !g_miniprof_tool_depth) ? preq_get(freq(req)) : 0;
    F_WRAP(START, pmpi_start_(req, ierr), b);
}

void mpi_startall_(F *count, F *reqs, F *ierr)
{
    long b = 0;
    if (g_mode && !g_miniprof_tool_depth)
        for (int i = 0; i < *count; i++) b += preq_get(freq(&reqs[i]));
    F_WRAP(STARTALL, pmpi_startall_(count, reqs, ierr), b);
}

void mpi_request_free_(F *req, F *ierr)
{
    preq_del(freq(req));
    pmpi_request_free_(req, ierr);
}

void mpi_mprobe_(F *source, F *tag, F *comm, F *msg, F *status, F *ierr)
{ F_WRAP(MPROBE, pmpi_mprobe_(source, tag, comm, msg, status, ierr), 0); }

void mpi_improbe_(F *source, F *tag, F *comm, F *flag, F *msg, F *status, F *ierr)
{ F_WRAP(IMPROBE, pmpi_improbe_(source, tag, comm, flag, msg, status, ierr), 0); }

void mpi_mrecv_(void *buf, F *count, F *dt, F *msg, F *status, F *ierr)
{ F_WRAP(MRECV, pmpi_mrecv_(buf, count, dt, msg, status, ierr), fbytes(count, dt)); }

void mpi_imrecv_(void *buf, F *count, F *dt, F *msg, F *req, F *ierr)
{ F_WRAP(IMRECV, pmpi_imrecv_(buf, count, dt, msg, req, ierr), fbytes(count, dt)); }

void mpi_ibarrier_(F *comm, F *req, F *ierr)
{ F_WRAP(IBARRIER, pmpi_ibarrier_(comm, req, ierr), 0); }

void mpi_ibcast_(void *buf, F *count, F *dt, F *root, F *comm, F *req, F *ierr)
{ F_WRAP(IBCAST, pmpi_ibcast_(buf, count, dt, root, comm, req, ierr), fbytes(count, dt)); }

void mpi_ireduce_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                  F *root, F *comm, F *req, F *ierr)
{ F_WRAP(IREDUCE, pmpi_ireduce_(sendbuf, recvbuf, count, dt, op, root, comm, req, ierr), fbytes(count, dt)); }

void mpi_iallreduce_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                     F *comm, F *req, F *ierr)
{ F_WRAP(IALLREDUCE, pmpi_iallreduce_(sendbuf, recvbuf, count, dt, op, comm, req, ierr), fbytes(count, dt)); }

void mpi_igather_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                  F *recvcount, F *recvtype, F *root, F *comm, F *req, F *ierr)
{
    F_WRAP(IGATHER,
           pmpi_igather_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                         recvtype, root, comm, req, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(recvcount, recvtype)
                                         : fbytes(sendcount, sendtype));
}

void mpi_igatherv_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                   F *recvcounts, F *displs, F *recvtype, F *root, F *comm,
                   F *req, F *ierr)
{
    F_WRAP(IGATHERV,
           pmpi_igatherv_(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                          displs, recvtype, root, comm, req, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(&recvcounts[*root], recvtype)
                                         : fbytes(sendcount, sendtype));
}

void mpi_iallgather_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                     F *recvcount, F *recvtype, F *comm, F *req, F *ierr)
{
    F_WRAP(IALLGATHER,
           pmpi_iallgather_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                            recvtype, comm, req, ierr),
           fbytes(recvcount, recvtype));
}

void mpi_iallgatherv_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                      F *recvcounts, F *displs, F *recvtype, F *comm, F *req, F *ierr)
{
    F_WRAP(IALLGATHERV,
           pmpi_iallgatherv_(sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                             displs, recvtype, comm, req, ierr),
           fbytes(&recvcounts[comm_rank(fcm(comm))], recvtype));
}

void mpi_iscatter_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                   F *recvcount, F *recvtype, F *root, F *comm, F *req, F *ierr)
{
    F_WRAP(ISCATTER,
           pmpi_iscatter_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                          recvtype, root, comm, req, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(sendcount, sendtype)
                                         : fbytes(recvcount, recvtype));
}

void mpi_iscatterv_(const void *sendbuf, F *sendcounts, F *displs, F *sendtype,
                    void *recvbuf, F *recvcount, F *recvtype, F *root, F *comm,
                    F *req, F *ierr)
{
    F_WRAP(ISCATTERV,
           pmpi_iscatterv_(sendbuf, sendcounts, displs, sendtype, recvbuf,
                           recvcount, recvtype, root, comm, req, ierr),
           comm_rank(fcm(comm)) == *root ? fbytes(&sendcounts[*root], sendtype)
                                         : fbytes(recvcount, recvtype));
}

void mpi_ialltoall_(const void *sendbuf, F *sendcount, F *sendtype, void *recvbuf,
                    F *recvcount, F *recvtype, F *comm, F *req, F *ierr)
{
    F_WRAP(IALLTOALL,
           pmpi_ialltoall_(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                           recvtype, comm, req, ierr),
           nbytes((long)*recvcount * comm_size(fcm(comm)), fdt(recvtype)));
}

void mpi_ialltoallv_(const void *sendbuf, F *sendcounts, F *sdispls, F *sendtype,
                     void *recvbuf, F *recvcounts, F *rdispls, F *recvtype,
                     F *comm, F *req, F *ierr)
{
    F_WRAP(IALLTOALLV,
           pmpi_ialltoallv_(sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                            recvcounts, rdispls, recvtype, comm, req, ierr),
           nbytes(fsum_counts(recvcounts, comm_size(fcm(comm))), fdt(recvtype)));
}

void mpi_ireduce_scatter_(const void *sendbuf, void *recvbuf, F *recvcounts,
                          F *dt, F *op, F *comm, F *req, F *ierr)
{
    F_WRAP(IREDUCE_SCATTER,
           pmpi_ireduce_scatter_(sendbuf, recvbuf, recvcounts, dt, op, comm, req, ierr),
           nbytes(fsum_counts(recvcounts, comm_size(fcm(comm))), fdt(dt)));
}

void mpi_ireduce_scatter_block_(const void *sendbuf, void *recvbuf, F *recvcount,
                                F *dt, F *op, F *comm, F *req, F *ierr)
{
    F_WRAP(IREDUCE_SCATTER_BLOCK,
           pmpi_ireduce_scatter_block_(sendbuf, recvbuf, recvcount, dt, op, comm, req, ierr),
           nbytes((long)*recvcount * comm_size(fcm(comm)), fdt(dt)));
}

void mpi_iscan_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                F *comm, F *req, F *ierr)
{ F_WRAP(ISCAN, pmpi_iscan_(sendbuf, recvbuf, count, dt, op, comm, req, ierr), fbytes(count, dt)); }

void mpi_iexscan_(const void *sendbuf, void *recvbuf, F *count, F *dt, F *op,
                  F *comm, F *req, F *ierr)
{ F_WRAP(IEXSCAN, pmpi_iexscan_(sendbuf, recvbuf, count, dt, op, comm, req, ierr), fbytes(count, dt)); }

void mpi_neighbor_allgather_(const void *sendbuf, F *sendcount, F *sendtype,
                             void *recvbuf, F *recvcount, F *recvtype, F *comm, F *ierr)
{
    F_WRAP(NEIGHBOR_ALLGATHER,
           pmpi_neighbor_allgather_(sendbuf, sendcount, sendtype, recvbuf,
                                    recvcount, recvtype, comm, ierr),
           nbytes((long)*recvcount * in_degree(fcm(comm)), fdt(recvtype)));
}

void mpi_neighbor_allgatherv_(const void *sendbuf, F *sendcount, F *sendtype,
                              void *recvbuf, F *recvcounts, F *displs, F *recvtype,
                              F *comm, F *ierr)
{
    F_WRAP(NEIGHBOR_ALLGATHERV,
           pmpi_neighbor_allgatherv_(sendbuf, sendcount, sendtype, recvbuf,
                                     recvcounts, displs, recvtype, comm, ierr),
           nbytes(fsum_counts(recvcounts, in_degree(fcm(comm))), fdt(recvtype)));
}

void mpi_neighbor_alltoall_(const void *sendbuf, F *sendcount, F *sendtype,
                            void *recvbuf, F *recvcount, F *recvtype, F *comm, F *ierr)
{
    F_WRAP(NEIGHBOR_ALLTOALL,
           pmpi_neighbor_alltoall_(sendbuf, sendcount, sendtype, recvbuf,
                                   recvcount, recvtype, comm, ierr),
           nbytes((long)*recvcount * in_degree(fcm(comm)), fdt(recvtype)));
}

void mpi_neighbor_alltoallv_(const void *sendbuf, F *sendcounts, F *sdispls,
                             F *sendtype, void *recvbuf, F *recvcounts,
                             F *rdispls, F *recvtype, F *comm, F *ierr)
{
    F_WRAP(NEIGHBOR_ALLTOALLV,
           pmpi_neighbor_alltoallv_(sendbuf, sendcounts, sdispls, sendtype,
                                    recvbuf, recvcounts, rdispls, recvtype,
                                    comm, ierr),
           nbytes(fsum_counts(recvcounts, in_degree(fcm(comm))), fdt(recvtype)));
}

void mpi_ineighbor_allgather_(const void *sendbuf, F *sendcount, F *sendtype,
                              void *recvbuf, F *recvcount, F *recvtype, F *comm,
                              F *req, F *ierr)
{
    F_WRAP(INEIGHBOR_ALLGATHER,
           pmpi_ineighbor_allgather_(sendbuf, sendcount, sendtype, recvbuf,
                                     recvcount, recvtype, comm, req, ierr),
           nbytes((long)*recvcount * in_degree(fcm(comm)), fdt(recvtype)));
}

void mpi_ineighbor_allgatherv_(const void *sendbuf, F *sendcount, F *sendtype,
                               void *recvbuf, F *recvcounts, F *displs,
                               F *recvtype, F *comm, F *req, F *ierr)
{
    F_WRAP(INEIGHBOR_ALLGATHERV,
           pmpi_ineighbor_allgatherv_(sendbuf, sendcount, sendtype, recvbuf,
                                      recvcounts, displs, recvtype, comm, req, ierr),
           nbytes(fsum_counts(recvcounts, in_degree(fcm(comm))), fdt(recvtype)));
}

void mpi_ineighbor_alltoall_(const void *sendbuf, F *sendcount, F *sendtype,
                             void *recvbuf, F *recvcount, F *recvtype, F *comm,
                             F *req, F *ierr)
{
    F_WRAP(INEIGHBOR_ALLTOALL,
           pmpi_ineighbor_alltoall_(sendbuf, sendcount, sendtype, recvbuf,
                                    recvcount, recvtype, comm, req, ierr),
           nbytes((long)*recvcount * in_degree(fcm(comm)), fdt(recvtype)));
}

void mpi_ineighbor_alltoallv_(const void *sendbuf, F *sendcounts, F *sdispls,
                              F *sendtype, void *recvbuf, F *recvcounts,
                              F *rdispls, F *recvtype, F *comm, F *req, F *ierr)
{
    F_WRAP(INEIGHBOR_ALLTOALLV,
           pmpi_ineighbor_alltoallv_(sendbuf, sendcounts, sdispls, sendtype,
                                     recvbuf, recvcounts, rdispls, recvtype,
                                     comm, req, ierr),
           nbytes(fsum_counts(recvcounts, in_degree(fcm(comm))), fdt(recvtype)));
}

void mpi_comm_dup_(F *comm, F *newcomm, F *ierr)
{ F_WRAP(COMM_DUP, pmpi_comm_dup_(comm, newcomm, ierr), 0); }

void mpi_comm_split_(F *comm, F *color, F *key, F *newcomm, F *ierr)
{ F_WRAP(COMM_SPLIT, pmpi_comm_split_(comm, color, key, newcomm, ierr), 0); }

void mpi_comm_split_type_(F *comm, F *split_type, F *key, F *info, F *newcomm, F *ierr)
{ F_WRAP(COMM_SPLIT_TYPE, pmpi_comm_split_type_(comm, split_type, key, info, newcomm, ierr), 0); }

void mpi_comm_create_(F *comm, F *group, F *newcomm, F *ierr)
{ F_WRAP(COMM_CREATE, pmpi_comm_create_(comm, group, newcomm, ierr), 0); }

void mpi_comm_create_group_(F *comm, F *group, F *tag, F *newcomm, F *ierr)
{ F_WRAP(COMM_CREATE_GROUP, pmpi_comm_create_group_(comm, group, tag, newcomm, ierr), 0); }

void mpi_comm_free_(F *comm, F *ierr)
{ F_WRAP(COMM_FREE, pmpi_comm_free_(comm, ierr), 0); }

/* LOGICAL arguments (periods, reorder, remain_dims) are passed through as
 * the library's own Fortran binding expects them */
void mpi_cart_create_(F *comm, F *ndims, F *dims, F *periods, F *reorder,
                      F *newcomm, F *ierr)
{ F_WRAP(CART_CREATE, pmpi_cart_create_(comm, ndims, dims, periods, reorder, newcomm, ierr), 0); }

void mpi_cart_sub_(F *comm, F *remain_dims, F *newcomm, F *ierr)
{ F_WRAP(CART_SUB, pmpi_cart_sub_(comm, remain_dims, newcomm, ierr), 0); }

void mpi_dist_graph_create_(F *comm, F *n, F *sources, F *degrees, F *destinations,
                            F *weights, F *info, F *reorder, F *newcomm, F *ierr)
{
    F_WRAP(DIST_GRAPH_CREATE,
           pmpi_dist_graph_create_(comm, n, sources, degrees, destinations,
                                   weights, info, reorder, newcomm, ierr), 0);
}

void mpi_dist_graph_create_adjacent_(F *comm, F *indegree, F *sources,
                                     F *sourceweights, F *outdegree,
                                     F *destinations, F *destweights, F *info,
                                     F *reorder, F *newcomm, F *ierr)
{
    F_WRAP(DIST_GRAPH_CREATE_ADJACENT,
           pmpi_dist_graph_create_adjacent_(comm, indegree, sources, sourceweights,
                                            outdegree, destinations, destweights,
                                            info, reorder, newcomm, ierr), 0);
}

void mpi_intercomm_create_(F *local_comm, F *local_leader, F *peer_comm,
                           F *remote_leader, F *tag, F *newintercomm, F *ierr)
{
    F_WRAP(INTERCOMM_CREATE,
           pmpi_intercomm_create_(local_comm, local_leader, peer_comm,
                                  remote_leader, tag, newintercomm, ierr), 0);
}

void mpi_intercomm_merge_(F *intercomm, F *high, F *newintracomm, F *ierr)
{ F_WRAP(INTERCOMM_MERGE, pmpi_intercomm_merge_(intercomm, high, newintracomm, ierr), 0); }

/* baseptr: INTEGER(KIND=MPI_ADDRESS_KIND), address-sized by reference */
void mpi_win_allocate_(MPI_Aint *size, F *disp_unit, F *info, F *comm,
                       MPI_Aint *baseptr, F *win, F *ierr)
{ F_WRAP(WIN_ALLOCATE, pmpi_win_allocate_(size, disp_unit, info, comm, baseptr, win, ierr), 0); }

void mpi_win_allocate_shared_(MPI_Aint *size, F *disp_unit, F *info, F *comm,
                              MPI_Aint *baseptr, F *win, F *ierr)
{ F_WRAP(WIN_ALLOCATE_SHARED, pmpi_win_allocate_shared_(size, disp_unit, info, comm, baseptr, win, ierr), 0); }

void mpi_win_create_dynamic_(F *info, F *comm, F *win, F *ierr)
{ F_WRAP(WIN_CREATE_DYNAMIC, pmpi_win_create_dynamic_(info, comm, win, ierr), 0); }

void mpi_win_lock_(F *lock_type, F *rank, F *assert, F *win, F *ierr)
{ F_WRAP(WIN_LOCK, pmpi_win_lock_(lock_type, rank, assert, win, ierr), 0); }

void mpi_win_unlock_(F *rank, F *win, F *ierr)
{ F_WRAP(WIN_UNLOCK, pmpi_win_unlock_(rank, win, ierr), 0); }

void mpi_win_lock_all_(F *assert, F *win, F *ierr)
{ F_WRAP(WIN_LOCK_ALL, pmpi_win_lock_all_(assert, win, ierr), 0); }

void mpi_win_unlock_all_(F *win, F *ierr)
{ F_WRAP(WIN_UNLOCK_ALL, pmpi_win_unlock_all_(win, ierr), 0); }

void mpi_win_flush_(F *rank, F *win, F *ierr)
{ F_WRAP(WIN_FLUSH, pmpi_win_flush_(rank, win, ierr), 0); }

void mpi_win_flush_all_(F *win, F *ierr)
{ F_WRAP(WIN_FLUSH_ALL, pmpi_win_flush_all_(win, ierr), 0); }

void mpi_win_flush_local_(F *rank, F *win, F *ierr)
{ F_WRAP(WIN_FLUSH_LOCAL, pmpi_win_flush_local_(rank, win, ierr), 0); }

void mpi_win_flush_local_all_(F *win, F *ierr)
{ F_WRAP(WIN_FLUSH_LOCAL_ALL, pmpi_win_flush_local_all_(win, ierr), 0); }

void mpi_win_sync_(F *win, F *ierr)
{ F_WRAP(WIN_SYNC, pmpi_win_sync_(win, ierr), 0); }

void mpi_win_post_(F *group, F *assert, F *win, F *ierr)
{ F_WRAP(WIN_POST, pmpi_win_post_(group, assert, win, ierr), 0); }

void mpi_win_start_(F *group, F *assert, F *win, F *ierr)
{ F_WRAP(WIN_START, pmpi_win_start_(group, assert, win, ierr), 0); }

void mpi_win_complete_(F *win, F *ierr)
{ F_WRAP(WIN_COMPLETE, pmpi_win_complete_(win, ierr), 0); }

void mpi_win_wait_(F *win, F *ierr)
{ F_WRAP(WIN_WAIT, pmpi_win_wait_(win, ierr), 0); }

void mpi_get_accumulate_(const void *origin, F *ocount, F *odt, void *result,
                         F *rcount, F *rdt, F *target_rank, MPI_Aint *target_disp,
                         F *tcount, F *tdt, F *op, F *win, F *ierr)
{
    F_WRAP(GET_ACCUMULATE,
           pmpi_get_accumulate_(origin, ocount, odt, result, rcount, rdt,
                                target_rank, target_disp, tcount, tdt, op,
                                win, ierr),
           fbytes(rcount, rdt));
}

void mpi_fetch_and_op_(const void *origin, void *result, F *dt, F *target_rank,
                       MPI_Aint *target_disp, F *op, F *win, F *ierr)
{
    F_WRAP(FETCH_AND_OP,
           pmpi_fetch_and_op_(origin, result, dt, target_rank, target_disp,
                              op, win, ierr),
           nbytes(1, fdt(dt)));
}

void mpi_compare_and_swap_(const void *origin, const void *compare, void *result,
                           F *dt, F *target_rank, MPI_Aint *target_disp,
                           F *win, F *ierr)
{
    F_WRAP(COMPARE_AND_SWAP,
           pmpi_compare_and_swap_(origin, compare, result, dt, target_rank,
                                  target_disp, win, ierr),
           nbytes(1, fdt(dt)));
}

void mpi_rput_(const void *origin, F *ocount, F *odt, F *target_rank,
               MPI_Aint *target_disp, F *tcount, F *tdt, F *win, F *req, F *ierr)
{
    F_WRAP(RPUT,
           pmpi_rput_(origin, ocount, odt, target_rank, target_disp, tcount,
                      tdt, win, req, ierr),
           fbytes(ocount, odt));
}

void mpi_rget_(void *origin, F *ocount, F *odt, F *target_rank,
               MPI_Aint *target_disp, F *tcount, F *tdt, F *win, F *req, F *ierr)
{
    F_WRAP(RGET,
           pmpi_rget_(origin, ocount, odt, target_rank, target_disp, tcount,
                      tdt, win, req, ierr),
           fbytes(ocount, odt));
}

void mpi_raccumulate_(const void *origin, F *ocount, F *odt, F *target_rank,
                      MPI_Aint *target_disp, F *tcount, F *tdt, F *op, F *win,
                      F *req, F *ierr)
{
    F_WRAP(RACCUMULATE,
           pmpi_raccumulate_(origin, ocount, odt, target_rank, target_disp,
                             tcount, tdt, op, win, req, ierr),
           fbytes(ocount, odt));
}
