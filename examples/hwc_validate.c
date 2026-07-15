/*
 * hwc_validate.c — sanity check for miniprof's Table 2 (hardware counter
 * characterisation)
 *
 * Two kernels with deliberately opposite cache/pipeline behaviour:
 *
 *   compute_bound_kernel() — several independent FP accumulator chains
 *                            (breaks the latency dependency a single
 *                            chain would have) over a working set of a
 *                            few doubles, entirely register/L1-resident.
 *                            Expect: high IPC, near-zero LLC misses.
 *
 *   memory_bound_kernel()  — pointer-chases a randomised permutation
 *                            cycle (Sattolo's algorithm — guarantees one
 *                            single N-length cycle, so every step is an
 *                            unpredictable jump, defeating the
 *                            prefetcher) over a buffer many times larger
 *                            than the last-level cache. Expect: low IPC,
 *                            high LLC misses.
 *
 * Each kernel runs in a wall-clock-bounded loop (not a fixed iteration
 * count) so it collects a comparable number of SIGPROF samples
 * regardless of the machine's clock speed.
 *
 * This is a manual validation aid, not part of the default `make all` —
 * see `make run-hwc-validate` at the repo root, which also greps the
 * resulting report and asserts the expected classification.
 */

#define _POSIX_C_SOURCE 199309L
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define KERNEL_SECONDS      3.0    /* wall time budget per kernel        */
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
        /* keep values bounded so this never over/underflows across the
           whole run — checked once per batch, not in the hot loop */
        for (int k = 0; k < 8; k++)
            if (x[k] > 1e30 || x[k] < -1e30) x[k] = 1.0 + 0.1 * k;
    } while (now_s() - t0 < budget_s);

    double sum = 0;
    for (int k = 0; k < 8; k++) sum += x[k];
    return sum + (double)batches * 0.0;  /* keep batches live for the optimiser */
}

/* ── memory-bound: pointer-chase over a buffer >> LLC ────────────────────── */
static long *build_chase_buffer(long n)
{
    long *idx = malloc((size_t)n * sizeof(long));
    long **buf = malloc((size_t)n * sizeof(long *));
    if (!idx || !buf) { fprintf(stderr, "hwc_validate: OOM\n"); MPI_Abort(MPI_COMM_WORLD, 1); }

    for (long i = 0; i < n; i++) idx[i] = i;

    /* Sattolo's algorithm: a random permutation with exactly one cycle
       of length n, so pointer-chasing visits every element exactly once
       before repeating — no short cycles for the CPU to learn/prefetch. */
    srand(12345);
    for (long i = n - 1; i > 0; i--) {
        long j = rand() % i;   /* strictly < i, unlike Fisher-Yates      */
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

    return (long)(uintptr_t)p + steps * 0;  /* keep p/steps live */
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0) {
        fprintf(stderr, "hwc_validate: building %.0f MiB pointer-chase buffer...\n",
                CHASE_ELEMENTS * sizeof(long) / (1024.0 * 1024.0));
    }
    long *chase = build_chase_buffer(CHASE_ELEMENTS);

    if (rank == 0) fprintf(stderr, "hwc_validate: running compute_bound_kernel...\n");
    double c = compute_bound_kernel(KERNEL_SECONDS);

    if (rank == 0) fprintf(stderr, "hwc_validate: running memory_bound_kernel...\n");
    long m = memory_bound_kernel(chase, KERNEL_SECONDS);

    free(chase);

    if (rank == 0) {
        fprintf(stderr, "hwc_validate: done (checksum %f / %ld, ignore — "
                        "just here to keep the optimiser honest)\n", c, m);
    }

    MPI_Finalize();
    return 0;
}
