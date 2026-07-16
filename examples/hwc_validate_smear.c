/*
 * hwc_validate_smear.c — sanity check for the Table 2 "interval smearing"
 * self-diagnostic in aggregator.c.
 *
 * Calls 20 distinct, noinline, near-instantaneous functions in a tight
 * round-robin for several seconds. Each call takes on the order of
 * nanoseconds — many thousands of calls happen inside any single
 * MINIPROF_INTERVAL_MS (default 10ms) sampling window. That's exactly
 * the condition where sample-mode hardware-counter attribution breaks
 * down: the counter delta read at each SIGPROF sample reflects
 * whichever mix of these 20 functions ran during the *whole* preceding
 * interval, not the one leaf PC the sample happens to land on — so
 * Cyc/samp should converge to roughly the same value (~clock_freq *
 * interval_ms) across all 20 functions, regardless of which one gets
 * the most samples. This is confirmed real-world behaviour (see the
 * NAS Parallel Benchmark BT data that motivated this detector), not a
 * hypothetical.
 *
 * Expected outcome: `make run-hwc-validate-smear` should trigger the
 * smearing warning in Table 2's report. This is the negative-control
 * counterpart to hwc_validate.c/hwc_validate_omp.c, which verify Table 2
 * stays *accurate* (and the warning stays *silent*) for long-running
 * functions.
 *
 * Not part of `make all` — see `make run-hwc-validate-smear` at the
 * repo root.
 */

#define _POSIX_C_SOURCE 199309L
#include <mpi.h>
#include <stdio.h>
#include <time.h>

#define N_FUNCS       20
#define RUN_SECONDS   5.0

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* 20 distinct, tiny, noinline functions — deliberately near-identical
 * in cost (a handful of arithmetic ops each) so any Table 2 difference
 * between them is purely a sampling artefact, not real work variation. */
#define DEFINE_TINY_FN(n) \
    static long __attribute__((noinline)) tiny_fn_##n(long x) \
    { return (x * 2654435761u) ^ (x >> 3) ^ n; }

DEFINE_TINY_FN(0)  DEFINE_TINY_FN(1)  DEFINE_TINY_FN(2)  DEFINE_TINY_FN(3)
DEFINE_TINY_FN(4)  DEFINE_TINY_FN(5)  DEFINE_TINY_FN(6)  DEFINE_TINY_FN(7)
DEFINE_TINY_FN(8)  DEFINE_TINY_FN(9)  DEFINE_TINY_FN(10) DEFINE_TINY_FN(11)
DEFINE_TINY_FN(12) DEFINE_TINY_FN(13) DEFINE_TINY_FN(14) DEFINE_TINY_FN(15)
DEFINE_TINY_FN(16) DEFINE_TINY_FN(17) DEFINE_TINY_FN(18) DEFINE_TINY_FN(19)

typedef long (*tiny_fn_t)(long);
static const tiny_fn_t g_fns[N_FUNCS] = {
    tiny_fn_0,  tiny_fn_1,  tiny_fn_2,  tiny_fn_3,  tiny_fn_4,
    tiny_fn_5,  tiny_fn_6,  tiny_fn_7,  tiny_fn_8,  tiny_fn_9,
    tiny_fn_10, tiny_fn_11, tiny_fn_12, tiny_fn_13, tiny_fn_14,
    tiny_fn_15, tiny_fn_16, tiny_fn_17, tiny_fn_18, tiny_fn_19,
};

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0)
        fprintf(stderr, "hwc_validate_smear: round-robin over %d tiny "
                        "functions for %.0fs...\n", N_FUNCS, RUN_SECONDS);

    long x = 42;
    double t0 = now_s();
    do {
        for (int rep = 0; rep < 100000; rep++) {
            for (int i = 0; i < N_FUNCS; i++) x = g_fns[i](x);
        }
    } while (now_s() - t0 < RUN_SECONDS);

    if (rank == 0)
        fprintf(stderr, "hwc_validate_smear: done (checksum %ld, ignore)\n", x);

    MPI_Finalize();
    return 0;
}
