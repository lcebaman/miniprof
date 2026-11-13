/*
 * omp_idle_validate.c — known answer for OpenMP worker idle accounting
 *
 * Each of ROUNDS rounds: a parallel region in which every thread spins W s,
 * then S s of serial work on the master while the workers wait in the
 * runtime's thread pool. With KMP_BLOCKTIME=infinite the workers spin the
 * whole time, so per rank, with T threads:
 *   compute = ROUNDS * (T * W + S)        (master: W + S, workers: W)
 *   idle    = ROUNDS * (T - 1) * S        (workers between regions)
 * Run with MINIPROF_COUNTERS_TEST=1: task-clock (CPU ns) stands in for
 * cycles, so these are CPU seconds. tools/validate_omp_idle.py checks them.
 * Needs an OpenMP runtime with OMPT (clang, AOCC, ICX); under GCC's libgomp
 * the validator reports SKIP.
 *
 * Usage: omp_idle_validate [ROUNDS W_MS S_MS]   (default 20 20 20)
 */
#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void spin(double s)
{
    double t0 = now();
    volatile double x = 0;
    while (now() - t0 < s) x += 1.0;
}

int main(int argc, char **argv)
{
    int prov;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &prov);
    int rounds = argc > 1 ? atoi(argv[1]) : 20;
    double w = (argc > 2 ? atof(argv[2]) : 20) * 1e-3;
    double s = (argc > 3 ? atof(argv[3]) : 20) * 1e-3;
    for (int r = 0; r < rounds; r++) {
        #pragma omp parallel
        spin(w);
        spin(s);
    }
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0)
        printf("omp_idle_validate: %d threads, %d rounds, W %.0f ms, S %.0f ms\n",
               omp_get_max_threads(), rounds, w * 1e3, s * 1e3);
    MPI_Finalize();
    return 0;
}
