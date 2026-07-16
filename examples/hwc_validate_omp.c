/*
 * hwc_validate_omp.c — sanity check for PER-THREAD hardware counters
 * under MPI+OpenMP.
 *
 * Same two kernels as hwc_validate.c (compute-bound FP chains vs.
 * memory-bound pointer-chase), but run CONCURRENTLY on two OpenMP
 * threads within a single MPI rank instead of sequentially on one.
 *
 * This specifically exercises the bug fixed in sampler_hwc.c: an
 * earlier version opened one perf_event_open counter group on whichever
 * thread called MPI_Init and read it from every thread's SIGPROF
 * handler unconditionally, so a sample landing in the OpenMP worker
 * thread running memory_bound_kernel would report the MPI_Init thread's
 * counters instead — collapsing both kernels' classification toward
 * whatever the (mostly idle, barrier-waiting) main thread happened to
 * be doing. With correct per-thread counter groups, this test expects
 * the SAME classification split as the sequential version:
 * compute_bound_kernel -> COMPUTE-BOUND, memory_bound_kernel ->
 * MEMORY-BOUND, despite both running at the same time on different
 * threads.
 *
 * Not part of `make all` — see `make run-hwc-validate-omp` at the repo
 * root.
 */

#define _POSIX_C_SOURCE 199309L
#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define KERNEL_SECONDS      4.0    /* wall time budget, run concurrently  */
#define CHASE_ELEMENTS  (32L * 1024 * 1024)  /* 32M * 8B = 256 MiB, >> LLC */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── compute-bound: independent FP chains, tiny working set ─────────────── */
static double compute_bound_kernel(double budget_s)
{
    double x[8] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7};
    const double a = 1.0000001, b = 0.9999999;
    double t0 = now_s();
    long   batches = 0;

    do {
        for (int rep = 0; rep < 1000000; rep++) {
            for (int k = 0; k < 8; k++) x[k] = x[k] * a + b;
        }
        batches++;
        for (int k = 0; k < 8; k++)
            if (x[k] > 1e30 || x[k] < -1e30) x[k] = 1.0 + 0.1 * k;
    } while (now_s() - t0 < budget_s);

    double sum = 0;
    for (int k = 0; k < 8; k++) sum += x[k];
    return sum + (double)batches * 0.0;
}

/* ── memory-bound: pointer-chase over a buffer >> LLC ────────────────────── */
static long *build_chase_buffer(long n)
{
    long *idx = malloc((size_t)n * sizeof(long));
    long **buf = malloc((size_t)n * sizeof(long *));
    if (!idx || !buf) { fprintf(stderr, "hwc_validate_omp: OOM\n"); MPI_Abort(MPI_COMM_WORLD, 1); }

    for (long i = 0; i < n; i++) idx[i] = i;

    srand(12345);
    for (long i = n - 1; i > 0; i--) {
        long j = rand() % i;
        long t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }

    for (long i = 0; i < n; i++) buf[i] = (long *)&buf[idx[i]];
    free(idx);
    return (long *)buf;
}

static long memory_bound_kernel(long *chase_start, double budget_s)
{
    long *p = chase_start;
    double t0 = now_s();
    long   steps = 0;

    do {
        for (int rep = 0; rep < 100000; rep++) {
            p = (long *)*p;
        }
        steps += 100000;
    } while (now_s() - t0 < budget_s);

    return (long)(uintptr_t)p + steps * 0;
}

int main(int argc, char **argv)
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0) {
        fprintf(stderr, "hwc_validate_omp: building %.0f MiB pointer-chase buffer...\n",
                CHASE_ELEMENTS * sizeof(long) / (1024.0 * 1024.0));
    }
    long *chase = build_chase_buffer(CHASE_ELEMENTS);

    if (rank == 0)
        fprintf(stderr, "hwc_validate_omp: running both kernels concurrently "
                        "on 2 OpenMP threads (this thread is NOT MPI_Init's "
                        "caller for either kernel)...\n");

    double c_result = 0;
    long   m_result = 0;

    #pragma omp parallel num_threads(2)
    {
        int tid = omp_get_thread_num();
        if (tid == 0) {
            double c = compute_bound_kernel(KERNEL_SECONDS);
            #pragma omp critical
            c_result = c;
        } else {
            long m = memory_bound_kernel(chase, KERNEL_SECONDS);
            #pragma omp critical
            m_result = m;
        }
    }

    free(chase);

    if (rank == 0) {
        fprintf(stderr, "hwc_validate_omp: done (checksum %f / %ld, ignore — "
                        "just here to keep the optimiser honest)\n",
                c_result, m_result);
    }

    MPI_Finalize();
    return 0;
}
