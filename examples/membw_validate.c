/*
 * membw_validate.c — known DRAM traffic for the snapshot memory block (P5)
 *
 * Every rank runs a triad a[i] = b[i] + s*c[i] over three arrays of N
 * doubles, REPS times. Arrays far larger than L3, so per repetition each
 * rank moves, to and from DRAM:
 *     reads  24 N bytes   (b, c, and the write-allocate read of a)
 *     writes  8 N bytes   (a, written back)
 * Initialisation adds about 24 N of reads and 24-48 N of writes once
 * (first touch, page zeroing), so with REPS = 40 the totals sit a few
 * percent above REPS x the per-repetition traffic.
 *
 * tools/validate_memory.py checks the snapshot's node totals against
 * this, and — when run with memory bound to the other socket
 * (numactl --membind) — that the remote-read share is near 100%.
 *
 * Usage: membw_validate [N [REPS]]     (defaults 32M doubles = 256 MiB/array, 40)
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

__attribute__((noinline)) static void triad(double *a, const double *b, const double *c,
                                            long n, double s)
{
    for (long i = 0; i < n; i++) a[i] = b[i] + s * c[i];
    __asm__ volatile("" ::: "memory");
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    long n = argc > 1 ? atol(argv[1]) : 32L * 1024 * 1024;
    int reps = argc > 2 ? atoi(argv[2]) : 40;

    double *a = malloc((size_t)n * sizeof(double));
    double *b = malloc((size_t)n * sizeof(double));
    double *c = malloc((size_t)n * sizeof(double));
    if (!a || !b || !c) { fprintf(stderr, "out of memory\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
    for (long i = 0; i < n; i++) { a[i] = 0.0; b[i] = 1.0; c[i] = 2.0; }

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = now();
    for (int r = 0; r < reps; r++) triad(a, b, c, n, 3.0);
    double dt = now() - t0;

    double mine = 32.0 * (double)n * reps / dt * 1e-9, sum = 0, chk = a[n / 2], chks = 0;
    MPI_Reduce(&mine, &sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&chk, &chks, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0)
        printf("membw_validate: %d ranks, N=%ld, reps=%d: per rank %.3f GB read + %.3f GB "
               "written in the loop; loop rate %.2f GB/s summed over ranks (check %g)\n",
               nprocs, n, reps, 24.0 * n * reps * 1e-9, 8.0 * n * reps * 1e-9, sum, chks);
    free(a); free(b); free(c);
    MPI_Finalize();
    return 0;
}
