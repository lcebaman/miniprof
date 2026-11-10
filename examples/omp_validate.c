/*
 * omp_validate.c — known-answer MPI+OpenMP workload for snapshot mode (P2)
 *
 * Phases (each in its own noinline function, so OMPT regions get distinct
 * names), with T OpenMP threads per rank:
 *
 *   serial_phase      main thread spins SERIAL_MS outside any parallel region
 *   balanced_phase    parallel: every thread spins BAL_MS
 *                     → imbalance ~0
 *   imbalanced_phase  parallel: thread t spins BASE_MS + t*DELTA_MS
 *                     → wall = BASE + (T-1)*DELTA
 *                     → barrier wait summed over threads = DELTA*T*(T-1)/2
 *                     → imbalance = wait / (T * wall)
 *   critical_phase    parallel: each thread enters a critical section
 *                     CRIT_ITERS times, spinning CRIT_MS inside
 *                     → lock wait > 0
 *   loop_phase        parallel for over 4*T iterations, schedule(static):
 *                     thread t gets the contiguous block [4t, 4t+4), each
 *                     iteration spinning (t+1)*LOOP_MS → same shape as
 *                     imbalanced_phase: imbalance = (T-1)/(2T)
 *
 * Each phase ends with an empty volatile asm: without it, a function whose
 * body is only a parallel construct compiles to a tail jump into the
 * runtime, and OMPT's return address then points into the caller (main),
 * so the regions would not get their own names.
 *
 * Each rank finishes with an MPI_Allreduce so the run is a real hybrid.
 * tools/validate_omp.py checks the snapshot against these expectations.
 *
 * Usage: omp_validate   (threads from OMP_NUM_THREADS)
 */

#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <time.h>

#define SERIAL_MS  200.0
#define BAL_MS     100.0
#define BASE_MS     50.0
#define DELTA_MS    50.0
#define CRIT_MS      2.0
#define CRIT_ITERS  20
#define LOOP_MS      5.0
#define NO_TAIL_CALL() __asm__ volatile("" ::: "memory")

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void spin_ms(double ms)
{
    double end = now() + ms * 1e-3;
    volatile double x = 1.0;
    while (now() < end) x = x * 1.0000001 + 1e-9;
}

__attribute__((noinline)) static void serial_phase(void) { spin_ms(SERIAL_MS); NO_TAIL_CALL(); }

__attribute__((noinline)) static void balanced_phase(void)
{
#pragma omp parallel
    spin_ms(BAL_MS);
    NO_TAIL_CALL();
}

__attribute__((noinline)) static void imbalanced_phase(void)
{
#pragma omp parallel
    spin_ms(BASE_MS + omp_get_thread_num() * DELTA_MS);
    NO_TAIL_CALL();
}

__attribute__((noinline)) static void critical_phase(void)
{
#pragma omp parallel
    for (int k = 0; k < CRIT_ITERS; k++) {
#pragma omp critical
        spin_ms(CRIT_MS);
    }
    NO_TAIL_CALL();
}

__attribute__((noinline)) static void loop_phase(void)
{
    int T = omp_get_max_threads();
#pragma omp parallel for schedule(static)
    for (int i = 0; i < 4 * T; i++) spin_ms((i / 4 + 1) * LOOP_MS);
    NO_TAIL_CALL();
}

int main(int argc, char **argv)
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    serial_phase();
    balanced_phase();
    imbalanced_phase();
    critical_phase();
    loop_phase();

    int one = 1, sum = 0;
    MPI_Allreduce(&one, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0)
        printf("omp_validate: %d ranks x %d threads, serial %.0f ms, bal %.0f ms, "
               "imbalanced %.0f+t*%.0f ms\n", sum, omp_get_max_threads(),
               SERIAL_MS, BAL_MS, BASE_MS, DELTA_MS);
    MPI_Finalize();
    return 0;
}
