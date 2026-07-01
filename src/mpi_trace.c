/*
 * mpi_trace.c — PMPI interposition for detailed MPI call tracing
 *
 * In trace mode, every wrapped MPI call here:
 *   1. Records entry time
 *   2. Calls through to PMPI_<Name>
 *   3. Records elapsed time + message bytes (where applicable)
 *
 * This matches CrayPat's `pat_build -g mpi` group tracing: per-function
 * call count, cumulative time, and message-size statistics.
 *
 * Only a representative, commonly-hot subset of MPI is wrapped here
 * (point-to-point + the collectives used in typical HPC codes). Adding
 * more is mechanical — same three-line pattern each time.
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
#include <string.h>
#include <time.h>
#include <stdatomic.h>

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
