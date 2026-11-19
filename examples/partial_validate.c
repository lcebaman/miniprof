/*
 * partial_validate.c — a run that never reaches MPI_Finalize.
 *
 * Every rank makes a known set of calls (K Allreduce of 8 doubles, 3
 * Barrier, one 4096-byte write to its own file), then ends as asked:
 *   term   print "ready <rank>" and compute until killed (the test sends
 *          SIGTERM to mpirun, as a batch time limit would)
 *   abort  the last rank calls MPI_Abort; the others wait in a Barrier
 *   exit   every rank calls exit(3) without MPI_Finalize
 *   handler as term, with the program's own SIGTERM handler installed
 *          before MPI_Init (checkpoint style): it must still run, after
 *          miniprof's record, and its exit code (5) must stand
 * tools/validate_partial.py checks the merged partial snapshot against
 * these counts. Usage: partial_validate term|abort|exit|handler [dir]
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <mpi.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define K 20

/* leaves <dir>/handler_ran.<pid>: a file, not output, because mpirun's
 * output forwarding may already be shutting down when SIGTERM arrives */
static char g_mark[1100];
static void app_handler(int sig)
{
    (void)sig;
    int fd = open(g_mark, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    _exit(5);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "handler") == 0) {
        snprintf(g_mark, sizeof g_mark, "%s/handler_ran.%d", argc > 2 ? argv[2] : ".", (int)getpid());
        signal(SIGTERM, app_handler);
    }
    MPI_Init(&argc, &argv);
    int me, n;
    MPI_Comm_rank(MPI_COMM_WORLD, &me);
    MPI_Comm_size(MPI_COMM_WORLD, &n);
    const char *mode = argc > 1 ? argv[1] : "term";
    const char *dir = argc > 2 ? argv[2] : ".";

    double a[8], b[8];
    for (int i = 0; i < 8; i++) a[i] = me;
    for (int k = 0; k < K; k++) MPI_Allreduce(a, b, 8, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    for (int k = 0; k < 3; k++) MPI_Barrier(MPI_COMM_WORLD);

    char path[1024], blk[4096];
    memset(blk, me, sizeof blk);
    snprintf(path, sizeof path, "%s/partial_validate.%d.tmp", dir, me);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, blk, sizeof blk) != (ssize_t)sizeof blk) { perror(path); MPI_Abort(MPI_COMM_WORLD, 2); }
    close(fd);
    unlink(path);

    if (strcmp(mode, "exit") == 0) exit(3);
    if (strcmp(mode, "abort") == 0) {
        if (me == n - 1) MPI_Abort(MPI_COMM_WORLD, 4);
        MPI_Barrier(MPI_COMM_WORLD);        /* never completes: the job is aborted */
    }
    /* term: compute, no MPI, until killed */
    printf("ready %d\n", me);
    fflush(stdout);
    volatile double x = 0;
    for (;;) for (int i = 0; i < 1000000; i++) x += i * 1e-9;
}
