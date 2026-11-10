/*
 * roofline_fma.c — compute-bound workload for the roofline check (P6)
 *
 * Every rank runs independent FMA chains in registers (no memory traffic)
 * for SECONDS, at the widest width the CPU has (AVX-512, else AVX2), so
 * the run sits far right of the ridge point and close to the calibrated
 * peak for that width when every physical core runs a rank.
 *
 * Usage: roofline_fma [SECONDS]   (default 3); prints the width used:
 * set MINIPROF_ROOFLINE_PEAK to match (fp64_512 or fp64_256).
 */
#include <immintrin.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAINS 12
static volatile double g_sink;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define FMA_RUN(NAME, TGT, VT, SET1, FMA)                                  \
__attribute__((target(TGT), noinline)) static void NAME(double sec)       \
{                                                                          \
    VT x[CHAINS], m = SET1(0.9999999), c = SET1(1e-9);                     \
    for (int k = 0; k < CHAINS; k++) x[k] = SET1(1.0 + k);                 \
    double t0 = now();                                                     \
    do {                                                                   \
        for (int r = 0; r < 4096; r++)                                     \
            for (int k = 0; k < CHAINS; k++) x[k] = FMA(x[k], m, c);       \
    } while (now() - t0 < sec);                                            \
    VT s = x[0];                                                           \
    for (int k = 1; k < CHAINS; k++) s = FMA(s, m, x[k]);                  \
    double out[8];                                                         \
    memcpy(out, &s, sizeof(s) < sizeof(out) ? sizeof(s) : sizeof(out));   \
    g_sink += out[0];                                                      \
}
FMA_RUN(run512, "avx512f", __m512d, _mm512_set1_pd, _mm512_fmadd_pd)
FMA_RUN(run256, "avx2,fma", __m256d, _mm256_set1_pd, _mm256_fmadd_pd)

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    double sec = argc > 1 ? atof(argv[1]) : 3.0;
    __builtin_cpu_init();
    int w512 = __builtin_cpu_supports("avx512f");
    MPI_Barrier(MPI_COMM_WORLD);
    if (w512) run512(sec); else run256(sec);
    if (rank == 0) printf("roofline_fma: width %s (MINIPROF_ROOFLINE_PEAK=%s)\n",
                          w512 ? "512" : "256", w512 ? "fp64_512" : "fp64_256");
    MPI_Finalize();
    return 0;
}
