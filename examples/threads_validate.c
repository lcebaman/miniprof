/*
 * threads_validate.c — many short-lived threads (audit finding C1).
 *
 * miniprof opens a counter group for every thread the application starts.
 * A thread that exits must give its file descriptors back, or an
 * application that creates short-lived threads runs out of descriptors
 * (its own open() fails with EMFILE), and its counts must still reach the
 * snapshot.
 *
 * Starts NTHREADS threads one after another; each burns BURN_MS of its own
 * CPU time (measured with CLOCK_THREAD_CPUTIME_ID) and exits. Prints, for
 * tools/validate_threads.py:
 *   threads=<n> fds_before=<n> fds_after=<n> open_ok=<0|1>
 *   thread_cpu_ns=<sum over the threads> process_cpu_ns=<whole process>
 * Run it under a low descriptor limit (ulimit -n 256): 300 threads x the
 * group's events is far above that if descriptors leak.
 *
 * Usage: threads_validate [nthreads [burn_ms]]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <mpi.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static long g_burn_ns = 2000000;

static uint64_t cpu_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *burn(void *out)
{
    uint64_t t0 = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    volatile double x = 0;
    while (cpu_ns(CLOCK_THREAD_CPUTIME_ID) - t0 < (uint64_t)g_burn_ns)
        for (int i = 0; i < 1000; i++) x += i * 0.5;
    *(uint64_t *)out = cpu_ns(CLOCK_THREAD_CPUTIME_ID);   /* whole thread, from its start */
    return NULL;
}

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n - 3;   /* ".", ".." and the DIR's own fd */
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int nthreads = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_burn_ns = atol(argv[2]) * 1000000L;

    int before = open_fds();
    uint64_t sum = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_t t;
        uint64_t used = 0;
        if (pthread_create(&t, NULL, burn, &used) != 0) { perror("pthread_create"); MPI_Abort(MPI_COMM_WORLD, 1); }
        pthread_join(t, NULL);
        sum += used;
    }
    int after = open_fds();

    /* the application must still be able to open files */
    int ok = 1, fds[8];
    for (int k = 0; k < 8; k++) {
        fds[k] = open("/proc/self/stat", O_RDONLY);
        if (fds[k] < 0) ok = 0;
    }
    for (int k = 0; k < 8; k++) if (fds[k] >= 0) close(fds[k]);

    uint64_t proc = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
    if (rank == 0)
        printf("threads=%d fds_before=%d fds_after=%d open_ok=%d thread_cpu_ns=%llu process_cpu_ns=%llu\n",
               nthreads, before, after, ok, (unsigned long long)sum, (unsigned long long)proc);
    MPI_Finalize();
    return 0;
}
