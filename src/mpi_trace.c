/*
 * mpi_trace.c — PMPI interposition for detailed MPI call tracing
 *
 * In trace mode, every wrapped MPI call here:
 *   1. Records entry time
 *   2. Calls through to PMPI_<Name>
 *   3. Records elapsed time + message bytes (where applicable)
 *
 * The report includes per-function call counts, cumulative time, and
 * message-size statistics.
 *
 * Coverage: point-to-point (Send/Recv/Sendrecv/Isend/Irecv), the full
 * Wait/Test family (Wait/Test, Waitany/Testany, Waitall/Testall),
 * collectives used in typical HPC codes (Barrier/Allreduce/Reduce/
 * Bcast/Gather/Allgather), and basic active-target one-sided RMA
 * (Win_create/Win_free/Win_fence/Put/Get/Accumulate). Not wrapped:
 * Waitsome/Testsome, passive-target RMA (lock/unlock), and
 * Get_accumulate/Compare_and_swap/Fetch_and_op — same three-line
 * pattern as everything else here, mechanical to add if needed.
 *
 * NOTE: these wrappers are only active in trace mode. In sample mode,
 * the linker still pulls in libmpi's real MPI_Xxx symbols directly since
 * PMPI_Xxx (not MPI_Xxx) is what these call through to — but to avoid
 * double-wrapping when both miniprof.c's MPI_Init/Finalize wrappers AND
 * these per-call wrappers are linked, mpi_trace.c independently checks
 * g_trace_mode at call time and short-circuits to a direct PMPI_* call
 * when not tracing, so there is zero added overhead in sample mode.
 */

#include "mpi_trace.h"
#include "trace_store.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* Fortran's MPI_STATUS_SIZE (the width of one status "block" in a Fortran
 * INTEGER array passed to array-of-statuses calls) is a Fortran-only
 * parameter (mpif-config.h), not exposed in the C mpi.h at all. The MPI
 * standard mandates every implementation define it, and every mainstream
 * one (Open MPI, MPICH, Intel MPI, Cray MPI) uses 6 — safe to hardcode
 * rather than parse a Fortran include file from C. */
#define MINIPROF_MPI_STATUS_SIZE 6

/* ── small fixed table: one slot per distinct MPI function name ─────────── */
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
static int                g_trace_mode = 0;

static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void mpi_trace_set_enabled(int enabled)
{
    g_trace_mode = enabled;
    if (enabled) {
        memset(g_mpi_table, 0, sizeof(g_mpi_table));
        g_mpi_n_used = 0;
    }
}

/* find-or-create slot by name (linear scan — table is tiny, <=64 entries) */
static mpi_trace_slot_t *get_slot(const char *name)
{
    for (int i = 0; i < g_mpi_n_used; i++)
        if (strcmp(g_mpi_table[i].name, name) == 0) return &g_mpi_table[i];

    if (g_mpi_n_used >= MPI_TRACE_MAX_FUNCS) return NULL; /* table full */
    mpi_trace_slot_t *s = &g_mpi_table[g_mpi_n_used++];
    snprintf(s->name, MINIPROF_SYMBOL_LEN, "%s", name);
    return s;
}

static void record(const char *name, double elapsed, long bytes)
{
    mpi_trace_slot_t *s = get_slot(name);
    if (s) {
        atomic_fetch_add_explicit(&s->calls, 1L, memory_order_relaxed);
        s->total_time_s += elapsed;
        s->total_bytes   += bytes;
    }

    /*
     * Feed this call's duration into trace_store's running MPI-time
     * counter, so any traced user function currently on the call stack
     * correctly excludes this time from its own "exclusive" total. See
     * trace_store.c's trace_store_exit() for how this is consumed.
     */
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

/* ── wrapped MPI calls ────────────────────────────────────────────────────── *
 * Pattern: if not tracing, call straight through with zero overhead.       */

int MPI_Send(const void *buf, int count, MPI_Datatype dt, int dest,
            int tag, MPI_Comm comm)
{
    if (!g_trace_mode) return PMPI_Send(buf, count, dt, dest, tag, comm);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Send(buf, count, dt, dest, tag, comm);
    record("MPI_Send", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Recv(void *buf, int count, MPI_Datatype dt, int source,
            int tag, MPI_Comm comm, MPI_Status *status)
{
    if (!g_trace_mode) return PMPI_Recv(buf, count, dt, source, tag, comm, status);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Recv(buf, count, dt, source, tag, comm, status);
    record("MPI_Recv", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int source, int recvtag,
                 MPI_Comm comm, MPI_Status *status)
{
    if (!g_trace_mode)
        return PMPI_Sendrecv(sendbuf, sendcount, sendtype, dest, sendtag,
                             recvbuf, recvcount, recvtype, source, recvtag,
                             comm, status);
    int dtsize; MPI_Type_size(sendtype, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Sendrecv(sendbuf, sendcount, sendtype, dest, sendtag,
                           recvbuf, recvcount, recvtype, source, recvtag,
                           comm, status);
    record("MPI_Sendrecv", now_s() - t0, (long)sendcount * dtsize);
    return rc;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype dt, int dest,
             int tag, MPI_Comm comm, MPI_Request *req)
{
    if (!g_trace_mode) return PMPI_Isend(buf, count, dt, dest, tag, comm, req);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Isend(buf, count, dt, dest, tag, comm, req);
    record("MPI_Isend", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Irecv(void *buf, int count, MPI_Datatype dt, int source,
             int tag, MPI_Comm comm, MPI_Request *req)
{
    if (!g_trace_mode) return PMPI_Irecv(buf, count, dt, source, tag, comm, req);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Irecv(buf, count, dt, source, tag, comm, req);
    record("MPI_Irecv", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Wait(MPI_Request *req, MPI_Status *status)
{
    if (!g_trace_mode) return PMPI_Wait(req, status);
    double t0 = now_s();
    int rc = PMPI_Wait(req, status);
    record("MPI_Wait", now_s() - t0, 0);
    return rc;
}

int MPI_Test(MPI_Request *req, int *flag, MPI_Status *status)
{
    if (!g_trace_mode) return PMPI_Test(req, flag, status);
    double t0 = now_s();
    int rc = PMPI_Test(req, flag, status);
    record("MPI_Test", now_s() - t0, 0);
    return rc;
}

int MPI_Waitall(int count, MPI_Request array_of_requests[],
               MPI_Status array_of_statuses[])
{
    if (!g_trace_mode) return PMPI_Waitall(count, array_of_requests, array_of_statuses);
    double t0 = now_s();
    int rc = PMPI_Waitall(count, array_of_requests, array_of_statuses);
    record("MPI_Waitall", now_s() - t0, 0);
    return rc;
}

int MPI_Waitany(int count, MPI_Request array_of_requests[], int *index,
               MPI_Status *status)
{
    if (!g_trace_mode) return PMPI_Waitany(count, array_of_requests, index, status);
    double t0 = now_s();
    int rc = PMPI_Waitany(count, array_of_requests, index, status);
    record("MPI_Waitany", now_s() - t0, 0);
    return rc;
}

int MPI_Testall(int count, MPI_Request array_of_requests[], int *flag,
                MPI_Status array_of_statuses[])
{
    if (!g_trace_mode) return PMPI_Testall(count, array_of_requests, flag, array_of_statuses);
    double t0 = now_s();
    int rc = PMPI_Testall(count, array_of_requests, flag, array_of_statuses);
    record("MPI_Testall", now_s() - t0, 0);
    return rc;
}

int MPI_Testany(int count, MPI_Request array_of_requests[], int *index,
                int *flag, MPI_Status *status)
{
    if (!g_trace_mode) return PMPI_Testany(count, array_of_requests, index, flag, status);
    double t0 = now_s();
    int rc = PMPI_Testany(count, array_of_requests, index, flag, status);
    record("MPI_Testany", now_s() - t0, 0);
    return rc;
}

int MPI_Barrier(MPI_Comm comm)
{
    if (!g_trace_mode) return PMPI_Barrier(comm);
    double t0 = now_s();
    int rc = PMPI_Barrier(comm);
    record("MPI_Barrier", now_s() - t0, 0);
    return rc;
}

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype dt, MPI_Op op, MPI_Comm comm)
{
    if (!g_trace_mode)
        return PMPI_Allreduce(sendbuf, recvbuf, count, dt, op, comm);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Allreduce(sendbuf, recvbuf, count, dt, op, comm);
    record("MPI_Allreduce", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
              MPI_Datatype dt, MPI_Op op, int root, MPI_Comm comm)
{
    if (!g_trace_mode)
        return PMPI_Reduce(sendbuf, recvbuf, count, dt, op, root, comm);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Reduce(sendbuf, recvbuf, count, dt, op, root, comm);
    record("MPI_Reduce", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Bcast(void *buf, int count, MPI_Datatype dt, int root, MPI_Comm comm)
{
    if (!g_trace_mode) return PMPI_Bcast(buf, count, dt, root, comm);
    int dtsize; MPI_Type_size(dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Bcast(buf, count, dt, root, comm);
    record("MPI_Bcast", now_s() - t0, (long)count * dtsize);
    return rc;
}

int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              int root, MPI_Comm comm)
{
    if (!g_trace_mode)
        return PMPI_Gather(sendbuf, sendcount, sendtype,
                           recvbuf, recvcount, recvtype, root, comm);
    int dtsize; MPI_Type_size(sendtype, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Gather(sendbuf, sendcount, sendtype,
                         recvbuf, recvcount, recvtype, root, comm);
    record("MPI_Gather", now_s() - t0, (long)sendcount * dtsize);
    return rc;
}

int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm)
{
    if (!g_trace_mode)
        return PMPI_Allgather(sendbuf, sendcount, sendtype,
                              recvbuf, recvcount, recvtype, comm);
    int dtsize; MPI_Type_size(sendtype, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Allgather(sendbuf, sendcount, sendtype,
                            recvbuf, recvcount, recvtype, comm);
    record("MPI_Allgather", now_s() - t0, (long)sendcount * dtsize);
    return rc;
}

/* ── one-sided RMA (active-target/fence synchronization only — passive-
 * target lock/unlock and Get_accumulate/Compare_and_swap/Fetch_and_op are
 * not wrapped here; same "mechanical to add more" note applies) ────────── */

int MPI_Win_create(void *base, MPI_Aint size, int disp_unit, MPI_Info info,
                   MPI_Comm comm, MPI_Win *win)
{
    if (!g_trace_mode)
        return PMPI_Win_create(base, size, disp_unit, info, comm, win);
    double t0 = now_s();
    int rc = PMPI_Win_create(base, size, disp_unit, info, comm, win);
    record("MPI_Win_create", now_s() - t0, 0);
    return rc;
}

int MPI_Win_free(MPI_Win *win)
{
    if (!g_trace_mode) return PMPI_Win_free(win);
    double t0 = now_s();
    int rc = PMPI_Win_free(win);
    record("MPI_Win_free", now_s() - t0, 0);
    return rc;
}

int MPI_Win_fence(int assert, MPI_Win win)
{
    if (!g_trace_mode) return PMPI_Win_fence(assert, win);
    double t0 = now_s();
    int rc = PMPI_Win_fence(assert, win);
    record("MPI_Win_fence", now_s() - t0, 0);
    return rc;
}

int MPI_Put(const void *origin_addr, int origin_count, MPI_Datatype origin_dt,
           int target_rank, MPI_Aint target_disp, int target_count,
           MPI_Datatype target_dt, MPI_Win win)
{
    if (!g_trace_mode)
        return PMPI_Put(origin_addr, origin_count, origin_dt, target_rank,
                        target_disp, target_count, target_dt, win);
    int dtsize; MPI_Type_size(origin_dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Put(origin_addr, origin_count, origin_dt, target_rank,
                      target_disp, target_count, target_dt, win);
    record("MPI_Put", now_s() - t0, (long)origin_count * dtsize);
    return rc;
}

int MPI_Get(void *origin_addr, int origin_count, MPI_Datatype origin_dt,
           int target_rank, MPI_Aint target_disp, int target_count,
           MPI_Datatype target_dt, MPI_Win win)
{
    if (!g_trace_mode)
        return PMPI_Get(origin_addr, origin_count, origin_dt, target_rank,
                        target_disp, target_count, target_dt, win);
    int dtsize; MPI_Type_size(origin_dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Get(origin_addr, origin_count, origin_dt, target_rank,
                      target_disp, target_count, target_dt, win);
    record("MPI_Get", now_s() - t0, (long)origin_count * dtsize);
    return rc;
}

int MPI_Accumulate(const void *origin_addr, int origin_count,
                   MPI_Datatype origin_dt, int target_rank,
                   MPI_Aint target_disp, int target_count,
                   MPI_Datatype target_dt, MPI_Op op, MPI_Win win)
{
    if (!g_trace_mode)
        return PMPI_Accumulate(origin_addr, origin_count, origin_dt,
                               target_rank, target_disp, target_count,
                               target_dt, op, win);
    int dtsize; MPI_Type_size(origin_dt, &dtsize);
    double t0 = now_s();
    int rc = PMPI_Accumulate(origin_addr, origin_count, origin_dt,
                             target_rank, target_disp, target_count,
                             target_dt, op, win);
    record("MPI_Accumulate", now_s() - t0, (long)origin_count * dtsize);
    return rc;
}

/* ── Fortran PMPI binding wrappers ───────────────────────────────────────── *
 *
 * Same rationale as the Fortran init/finalize wrappers in miniprof.c:
 * Fortran's mpi module calls separately-mangled symbols (mpi_send_,
 * mpi_allreduce_, etc.) that bypass the C wrappers above entirely.
 * Confirmed by disassembly: a Fortran MPI_Allreduce call site targets
 * mpi_allreduce_@plt, never MPI_Allreduce.
 *
 * Rather than reimplementing the timing/recording logic per binding,
 * each wrapper here is a thin adapter: convert Fortran INTEGER handles
 * (MPI_Fint) to real C handles via the standard *_f2c() API, call the
 * matching C wrapper above (which already has all the timing logic),
 * then convert any output handles back via *_c2f(). This means the
 * timing/grouping logic only exists in one place per call.
 *
 * MPI_Status deserves a specific note: in Fortran it's an INTEGER array
 * of MPI_STATUS_SIZE elements, not the C struct: MPI_Status_f2c() /
 * MPI_Status_c2f() handle that conversion. MPI_STATUS_IGNORE is passed
 * by Fortran as a special sentinel array; we forward a NULL/ignore
 * pointer through to the C status normally, falling back safely if the
 * sentinel can't be distinguished (matches Open MPI's own bindings
 * approach in libmpi_mpifh.so for the same problem).
 *
 * Defined with trailing-underscore mangling (gfortran's default — see
 * the matching note in miniprof.c's Fortran wrappers for other
 * compilers' naming conventions).
 */

void mpi_send_(const void *buf, MPI_Fint *count, MPI_Fint *datatype,
               MPI_Fint *dest, MPI_Fint *tag, MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Send(buf, *count, dt, *dest, *tag, cm);
}

void mpi_recv_(void *buf, MPI_Fint *count, MPI_Fint *datatype,
               MPI_Fint *source, MPI_Fint *tag, MPI_Fint *comm,
               MPI_Fint *status, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    MPI_Status   c_status;
    *ierr = MPI_Recv(buf, *count, dt, *source, *tag, cm, &c_status);
    MPI_Status_c2f(&c_status, status);
}

void mpi_sendrecv_(const void *sendbuf, MPI_Fint *sendcount, MPI_Fint *sendtype,
                   MPI_Fint *dest, MPI_Fint *sendtag,
                   void *recvbuf, MPI_Fint *recvcount, MPI_Fint *recvtype,
                   MPI_Fint *source, MPI_Fint *recvtag,
                   MPI_Fint *comm, MPI_Fint *status, MPI_Fint *ierr)
{
    MPI_Datatype st = MPI_Type_f2c(*sendtype);
    MPI_Datatype rt = MPI_Type_f2c(*recvtype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    MPI_Status   c_status;
    *ierr = MPI_Sendrecv(sendbuf, *sendcount, st, *dest, *sendtag,
                         recvbuf, *recvcount, rt, *source, *recvtag,
                         cm, &c_status);
    MPI_Status_c2f(&c_status, status);
}

void mpi_isend_(const void *buf, MPI_Fint *count, MPI_Fint *datatype,
                MPI_Fint *dest, MPI_Fint *tag, MPI_Fint *comm,
                MPI_Fint *request, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    MPI_Request  c_req;
    *ierr = MPI_Isend(buf, *count, dt, *dest, *tag, cm, &c_req);
    *request = MPI_Request_c2f(c_req);
}

void mpi_irecv_(void *buf, MPI_Fint *count, MPI_Fint *datatype,
                MPI_Fint *source, MPI_Fint *tag, MPI_Fint *comm,
                MPI_Fint *request, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    MPI_Request  c_req;
    *ierr = MPI_Irecv(buf, *count, dt, *source, *tag, cm, &c_req);
    *request = MPI_Request_c2f(c_req);
}

void mpi_wait_(MPI_Fint *request, MPI_Fint *status, MPI_Fint *ierr)
{
    MPI_Request c_req = MPI_Request_f2c(*request);
    MPI_Status  c_status;
    *ierr = MPI_Wait(&c_req, &c_status);
    *request = MPI_Request_c2f(c_req);
    MPI_Status_c2f(&c_status, status);
}

void mpi_barrier_(MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Comm cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Barrier(cm);
}

void mpi_allreduce_(const void *sendbuf, void *recvbuf, MPI_Fint *count,
                    MPI_Fint *datatype, MPI_Fint *op, MPI_Fint *comm,
                    MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Op       o  = MPI_Op_f2c(*op);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Allreduce(sendbuf, recvbuf, *count, dt, o, cm);
}

void mpi_reduce_(const void *sendbuf, void *recvbuf, MPI_Fint *count,
                 MPI_Fint *datatype, MPI_Fint *op, MPI_Fint *root,
                 MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Op       o  = MPI_Op_f2c(*op);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Reduce(sendbuf, recvbuf, *count, dt, o, *root, cm);
}

void mpi_bcast_(void *buf, MPI_Fint *count, MPI_Fint *datatype,
                MPI_Fint *root, MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Datatype dt = MPI_Type_f2c(*datatype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Bcast(buf, *count, dt, *root, cm);
}

void mpi_gather_(const void *sendbuf, MPI_Fint *sendcount, MPI_Fint *sendtype,
                 void *recvbuf, MPI_Fint *recvcount, MPI_Fint *recvtype,
                 MPI_Fint *root, MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Datatype st = MPI_Type_f2c(*sendtype);
    MPI_Datatype rt = MPI_Type_f2c(*recvtype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Gather(sendbuf, *sendcount, st,
                       recvbuf, *recvcount, rt, *root, cm);
}

void mpi_allgather_(const void *sendbuf, MPI_Fint *sendcount, MPI_Fint *sendtype,
                    void *recvbuf, MPI_Fint *recvcount, MPI_Fint *recvtype,
                    MPI_Fint *comm, MPI_Fint *ierr)
{
    MPI_Datatype st = MPI_Type_f2c(*sendtype);
    MPI_Datatype rt = MPI_Type_f2c(*recvtype);
    MPI_Comm     cm = MPI_Comm_f2c(*comm);
    *ierr = MPI_Allgather(sendbuf, *sendcount, st, recvbuf, *recvcount, rt, cm);
}

void mpi_test_(MPI_Fint *request, MPI_Fint *flag, MPI_Fint *status, MPI_Fint *ierr)
{
    MPI_Request c_req = MPI_Request_f2c(*request);
    MPI_Status  c_status;
    int         c_flag;
    *ierr = MPI_Test(&c_req, &c_flag, &c_status);
    *flag    = c_flag ? 1 : 0;
    *request = MPI_Request_c2f(c_req);
    MPI_Status_c2f(&c_status, status);
}

/*
 * Array-of-requests/statuses bindings (Waitall/Waitany/Testall/Testany):
 * Fortran passes a flat INTEGER array for requests and a flat INTEGER
 * array for statuses, one MINIPROF_MPI_STATUS_SIZE-wide block per status
 * (see the constant's doc comment above). We heap-allocate scratch C
 * arrays sized to the caller's `count` — a stack VLA would also work but
 * risks stack exhaustion for codes with very large in-flight request
 * counts, which one-sided/many-neighbor halo codes can genuinely have.
 */

void mpi_waitall_(MPI_Fint *count, MPI_Fint *array_of_requests,
                  MPI_Fint *array_of_statuses, MPI_Fint *ierr)
{
    int n = (int)*count;
    MPI_Request *reqs  = malloc((size_t)n * sizeof(MPI_Request));
    MPI_Status  *stats = malloc((size_t)n * sizeof(MPI_Status));
    for (int i = 0; i < n; i++) reqs[i] = MPI_Request_f2c(array_of_requests[i]);

    *ierr = MPI_Waitall(n, reqs, stats);

    for (int i = 0; i < n; i++) {
        array_of_requests[i] = MPI_Request_c2f(reqs[i]);
        MPI_Status_c2f(&stats[i], &array_of_statuses[i * MINIPROF_MPI_STATUS_SIZE]);
    }
    free(reqs);
    free(stats);
}

void mpi_waitany_(MPI_Fint *count, MPI_Fint *array_of_requests, MPI_Fint *index,
                  MPI_Fint *status, MPI_Fint *ierr)
{
    int n = (int)*count;
    MPI_Request *reqs = malloc((size_t)n * sizeof(MPI_Request));
    for (int i = 0; i < n; i++) reqs[i] = MPI_Request_f2c(array_of_requests[i]);

    int        c_index;
    MPI_Status c_status;
    *ierr = MPI_Waitany(n, reqs, &c_index, &c_status);

    for (int i = 0; i < n; i++) array_of_requests[i] = MPI_Request_c2f(reqs[i]);
    /* Fortran indices are 1-based; MPI_UNDEFINED (all requests were
       MPI_REQUEST_NULL) is a fixed sentinel, not a real index — leave it
       untranslated, matching the standard's own Fortran binding. */
    *index = (c_index == MPI_UNDEFINED) ? MPI_UNDEFINED : c_index + 1;
    MPI_Status_c2f(&c_status, status);
    free(reqs);
}

void mpi_testall_(MPI_Fint *count, MPI_Fint *array_of_requests, MPI_Fint *flag,
                  MPI_Fint *array_of_statuses, MPI_Fint *ierr)
{
    int n = (int)*count;
    MPI_Request *reqs  = malloc((size_t)n * sizeof(MPI_Request));
    MPI_Status  *stats = malloc((size_t)n * sizeof(MPI_Status));
    for (int i = 0; i < n; i++) reqs[i] = MPI_Request_f2c(array_of_requests[i]);

    int c_flag;
    *ierr = MPI_Testall(n, reqs, &c_flag, stats);
    *flag = c_flag ? 1 : 0;

    for (int i = 0; i < n; i++) {
        array_of_requests[i] = MPI_Request_c2f(reqs[i]);
        MPI_Status_c2f(&stats[i], &array_of_statuses[i * MINIPROF_MPI_STATUS_SIZE]);
    }
    free(reqs);
    free(stats);
}

void mpi_testany_(MPI_Fint *count, MPI_Fint *array_of_requests, MPI_Fint *index,
                  MPI_Fint *flag, MPI_Fint *status, MPI_Fint *ierr)
{
    int n = (int)*count;
    MPI_Request *reqs = malloc((size_t)n * sizeof(MPI_Request));
    for (int i = 0; i < n; i++) reqs[i] = MPI_Request_f2c(array_of_requests[i]);

    int        c_index, c_flag;
    MPI_Status c_status;
    *ierr = MPI_Testany(n, reqs, &c_index, &c_flag, &c_status);

    for (int i = 0; i < n; i++) array_of_requests[i] = MPI_Request_c2f(reqs[i]);
    *index = (c_index == MPI_UNDEFINED) ? MPI_UNDEFINED : c_index + 1;
    *flag  = c_flag ? 1 : 0;
    MPI_Status_c2f(&c_status, status);
    free(reqs);
}

/*
 * RMA bindings. target_disp (and MPI_Win_create's `size`) are MPI_Aint —
 * in Fortran that's INTEGER(KIND=MPI_ADDRESS_KIND), an address-sized
 * integer distinct from the plain-INTEGER MPI_Fint used everywhere else
 * in this file. We take it as `MPI_Aint *` directly (not `MPI_Fint *`)
 * and dereference it as-is: unlike MPI_Comm/MPI_Request/MPI_Win, MPI_Aint
 * is not an opaque handle needing f2c/c2f translation — it's the same
 * integer in both languages, gfortran just widens it to match address
 * width. MPI_Win itself IS an opaque handle and does need MPI_Win_f2c/c2f.
 */

void mpi_win_create_(void *base, MPI_Aint *size, MPI_Fint *disp_unit,
                     MPI_Fint *info, MPI_Fint *comm, MPI_Fint *win,
                     MPI_Fint *ierr)
{
    MPI_Info info_c = MPI_Info_f2c(*info);
    MPI_Comm comm_c = MPI_Comm_f2c(*comm);
    MPI_Win  win_c;
    *ierr = MPI_Win_create(base, *size, (int)*disp_unit, info_c, comm_c, &win_c);
    *win  = MPI_Win_c2f(win_c);
}

void mpi_win_free_(MPI_Fint *win, MPI_Fint *ierr)
{
    MPI_Win win_c = MPI_Win_f2c(*win);
    *ierr = MPI_Win_free(&win_c);
    *win  = MPI_Win_c2f(win_c); /* becomes MPI_WIN_NULL */
}

void mpi_win_fence_(MPI_Fint *assert, MPI_Fint *win, MPI_Fint *ierr)
{
    MPI_Win win_c = MPI_Win_f2c(*win);
    *ierr = MPI_Win_fence((int)*assert, win_c);
}

void mpi_put_(const void *origin_addr, MPI_Fint *origin_count,
             MPI_Fint *origin_datatype, MPI_Fint *target_rank,
             MPI_Aint *target_disp, MPI_Fint *target_count,
             MPI_Fint *target_datatype, MPI_Fint *win, MPI_Fint *ierr)
{
    MPI_Datatype odt   = MPI_Type_f2c(*origin_datatype);
    MPI_Datatype tdt   = MPI_Type_f2c(*target_datatype);
    MPI_Win      win_c = MPI_Win_f2c(*win);
    *ierr = MPI_Put(origin_addr, *origin_count, odt, *target_rank,
                    *target_disp, *target_count, tdt, win_c);
}

void mpi_get_(void *origin_addr, MPI_Fint *origin_count,
             MPI_Fint *origin_datatype, MPI_Fint *target_rank,
             MPI_Aint *target_disp, MPI_Fint *target_count,
             MPI_Fint *target_datatype, MPI_Fint *win, MPI_Fint *ierr)
{
    MPI_Datatype odt   = MPI_Type_f2c(*origin_datatype);
    MPI_Datatype tdt   = MPI_Type_f2c(*target_datatype);
    MPI_Win      win_c = MPI_Win_f2c(*win);
    *ierr = MPI_Get(origin_addr, *origin_count, odt, *target_rank,
                    *target_disp, *target_count, tdt, win_c);
}

void mpi_accumulate_(const void *origin_addr, MPI_Fint *origin_count,
                     MPI_Fint *origin_datatype, MPI_Fint *target_rank,
                     MPI_Aint *target_disp, MPI_Fint *target_count,
                     MPI_Fint *target_datatype, MPI_Fint *op, MPI_Fint *win,
                     MPI_Fint *ierr)
{
    MPI_Datatype odt   = MPI_Type_f2c(*origin_datatype);
    MPI_Datatype tdt   = MPI_Type_f2c(*target_datatype);
    MPI_Op       op_c  = MPI_Op_f2c(*op);
    MPI_Win      win_c = MPI_Win_f2c(*win);
    *ierr = MPI_Accumulate(origin_addr, *origin_count, odt, *target_rank,
                           *target_disp, *target_count, tdt, op_c, win_c);
}
