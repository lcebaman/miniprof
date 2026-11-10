/*
 * counters_validate.c — known-answer workload for snapshot-mode counters (P3)
 *
 * Every FP operation in a region is known exactly, so the FLOP counts that
 * miniprof attributes can be checked to the last few ppm:
 *
 *   region "daxpy"       y = a*x + y,  N elements x REPS        2*N*REPS flops
 *   region "dot_scalar"  s += x*y, vectorisation disabled       2*N*REPS flops
 *                        (scalar ops only: N*REPS if the compiler fuses
 *                         into FMA, 2*N*REPS if not)
 *   region "outer"       { region "inner" { daxpy REPS/2 } ;
 *                          daxpy REPS/2 }            outer 2*N*REPS, inner N*REPS
 *   region "thread_work" 2 pthreads created after MPI_Init, each daxpy REPS:
 *                        4*N*REPS flops per rank (checks the pthread_create
 *                        interposer and per-thread groups)
 *   MPI phase            STEPS x (MPI_Allreduce + MPI_Barrier), no FP work
 *                        between them, so FLOPs inside MPI should be ~0
 *
 * Outside regions: array initialisation (N multiplies per array) and a
 * checksum (N adds) — small and bounded, so the run total is checked as a
 * range. tools/validate_counters.py checks the snapshot JSON and, with
 * --perf-flops, compares the total against `perf stat` on the same binary.
 *
 * Usage: counters_validate [N REPS]   (default 2097152 20)
 */

#define _GNU_SOURCE
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/miniprof.h"

static long N = 2097152;
static int  REPS = 20;

/* noinline + no-tree-vectorize (GCC) / loop pragma (clang, AOCC) keep the
 * FP work exactly as written. Strict FP semantics (no -ffast-math) also
 * stop either compiler from vectorising the dot-product reduction. */
__attribute__((noinline))
static void daxpy(double a, const double *x, double *y, long n, int reps)
{
    for (int r = 0; r < reps; r++)
        for (long i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}

__attribute__((noinline, optimize("no-tree-vectorize")))
static double dot_scalar(const double *x, const double *y, long n, int reps)
{
    double s = 0.0;
    for (int r = 0; r < reps; r++)
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable)
#endif
        for (long i = 0; i < n; i++) s += x[i] * y[i];
    return s;
}

typedef struct { double *x, *y; } targ_t;

static void *thread_body(void *p)
{
    targ_t *t = p;
    miniprof_region_begin("thread_work");
    daxpy(1.0000001, t->x, t->y, N, REPS);
    miniprof_region_end("thread_work");
    return NULL;
}

static double *alloc_init(double scale)
{
    double *v = malloc((size_t)N * sizeof(double));
    if (!v) { fprintf(stderr, "out of memory\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
    for (long i = 0; i < N; i++) v[i] = (double)i * scale;   /* 1 flop each */
    return v;
}

int main(int argc, char **argv)
{
    if (argc > 1) N = atol(argv[1]);
    if (argc > 2) REPS = atoi(argv[2]);

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    double *x = alloc_init(1e-9), *y = alloc_init(1e-9);

    miniprof_region_begin("daxpy");
    daxpy(1.0000001, x, y, N, REPS);
    miniprof_region_end("daxpy");

    miniprof_region_begin("dot_scalar");
    volatile double d = dot_scalar(x, y, N, REPS);
    miniprof_region_end("dot_scalar");
    (void)d;

    miniprof_region_begin("outer");
    miniprof_region_begin("inner");
    daxpy(1.0000001, x, y, N, REPS / 2);
    miniprof_region_end("inner");
    daxpy(1.0000001, x, y, N, REPS - REPS / 2);
    miniprof_region_end("outer");

    /* two worker threads, each with its own arrays */
    pthread_t th[2];
    targ_t ta[2];
    for (int t = 0; t < 2; t++) {
        ta[t].x = alloc_init(1e-9);
        ta[t].y = alloc_init(1e-9);
        pthread_create(&th[t], NULL, thread_body, &ta[t]);
    }
    for (int t = 0; t < 2; t++) pthread_join(th[t], NULL);

    /* MPI phase: no FP work between calls */
    for (int s = 0; s < 200; s++) {
        long v = s, r = 0;
        MPI_Allreduce(&v, &r, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
    }

    double sum = 0.0;
    for (long i = 0; i < N; i++) sum += y[i];                  /* N flops */

    if (rank == 0)
        printf("counters_validate: %d ranks, N=%ld, REPS=%d, checksum %.6g\n",
               nprocs, N, REPS, sum);
    for (int t = 0; t < 2; t++) { free(ta[t].x); free(ta[t].y); }
    free(x); free(y);
    MPI_Finalize();
    return 0;
}
