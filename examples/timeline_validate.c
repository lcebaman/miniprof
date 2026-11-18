/*
 * timeline_validate.c — known phases for the snapshot timeline (P7)
 *
 * Three phases of PHASE seconds each (default 0.6), wall-clock timed:
 *   A  compute only, no MPI                     every rank: MPI ~0%
 *   B  steps of compute + MPI_Allreduce; rank 0 computes 10x longer than
 *      the others, which wait for it inside the allreduce:
 *                                               rank 0 MPI low, others high
 *   C  as B with the slow rank moved to the last rank:
 *                                               last rank low, others high
 * tools/validate_timeline.py checks these shapes in the snapshot's
 * timeline block (run with MINIPROF_TIMELINE=1, >= 2 ranks).
 *
 * Usage: timeline_validate [PHASE_SECONDS]
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

static volatile double g_x = 1.0;
static void spin(double s)
{
    double end = now() + s;
    while (now() < end)
        for (int i = 0; i < 1000; i++) g_x = g_x * 1.0000001 + 1e-9;
}

/* steps of compute + allreduce for `phase` seconds; `slow` computes 10x */
static void imbalanced(double phase, int rank, int slow)
{
    double step = 0.03, end = now() + phase;
    int go = 1;
    while (go) {
        spin(rank == slow ? step : step / 10);
        double v = now() < end ? 1.0 : 0.0, all = 0;
        MPI_Allreduce(&v, &all, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
        go = all > 0;     /* every rank leaves together */
    }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    double phase = argc > 1 ? atof(argv[1]) : 0.6;

    spin(phase);                          /* A */
    imbalanced(phase, rank, 0);           /* B */
    imbalanced(phase, rank, size - 1);    /* C */

    if (rank == 0)
        printf("timeline_validate: %d ranks, 3 phases of %.2f s (A compute, B rank 0 slow, "
               "C rank %d slow)\n", size, phase, size - 1);
    MPI_Finalize();
    return 0;
}
