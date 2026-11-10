/*
 * mpi_percall.c — per-call cost of miniprof's MPI wrappers
 *
 * Times ITERS calls of MPI_Iprobe on a tag nobody sends (a local call
 * with no communication), so the difference between MINIPROF_MODE=off and
 * a profiling mode is the wrapper's own cost: timing, accounting and, in
 * snapshot mode, the two counter reads per call (rdpmc, or read() with
 * MINIPROF_NO_RDPMC=1).
 *
 * Usage: mpi_percall [ITERS]   (default 2000000); rank 0 prints ns/call.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 2000000;
    MPI_Init(&argc, &argv);
    int rank, flag;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    for (long i = 0; i < iters / 10; i++)     /* warm-up */
        MPI_Iprobe(MPI_ANY_SOURCE, 32000, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    double t = MPI_Wtime();
    for (long i = 0; i < iters; i++)
        MPI_Iprobe(MPI_ANY_SOURCE, 32000, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    t = MPI_Wtime() - t;
    if (rank == 0) printf("%.1f ns/call\n", t / iters * 1e9);
    MPI_Finalize();
    return 0;
}
