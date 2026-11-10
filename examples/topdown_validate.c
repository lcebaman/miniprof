/*
 * topdown_validate.c — known-shape workload for snapshot top-down (P4)
 *
 * Five regions, each built so one top-down category dominates; every rank
 * runs the same thing. Run with >= 3 ranks (or threads) so each of the
 * three event sets is measured (see counters.h).
 *
 *   alu      independent integer chains, fits in registers
 *            → retiring dominates
 *   fpchain  one dependent chain of FP adds (latency-bound, no memory)
 *            → backend bound, almost all of it backend_cpu
 *   chase    pointer chase through a random cycle far larger than L3
 *            → backend bound, almost all of it backend_memory
 *   stream   triad a[i] = b[i] + s*c[i] over arrays larger than L3
 *            → backend bound, mostly backend_memory
 *   branch   data-dependent branch on random bits (50/50, unpredictable)
 *            → bad speculation, almost all of it mispredicts
 *
 * There is no frontend-bound kernel: a reliable one needs a code
 * footprint larger than the op cache and i-cache, which is not portable.
 *
 * Each region repeats its kernel until TARGET_MS has passed, so regions
 * weigh about the same. tools/validate_topdown.py checks the shapes;
 * tools/validate_topdown.sh compares with `perf stat -M PipelineL1/L2`.
 *
 * Usage: topdown_validate [scale]   (scale 1 = node sizes; 0.05 for CI)
 */

#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "miniprof.h"

#define TARGET_MS 300.0

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static volatile uint64_t g_sink;   /* keeps results alive */

static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

__attribute__((noinline)) static void alu(long n)
{
    uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    for (long i = 0; i < n; i++) {
        a += i; b ^= i; c += 3; d ^= 5; e += 7; f ^= 9; g += 11; h ^= 13;
        __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d),
                              "+r"(e), "+r"(f), "+r"(g), "+r"(h));
    }
    g_sink = a + b + c + d + e + f + g + h;
}

__attribute__((noinline)) static void fpchain(long n)
{
    double x = 1.0;
    for (long i = 0; i < n; i++) {
        x = x * 0.9999999 + 1e-9;           /* one dependent chain */
        __asm__ volatile("" : "+x"(x));
    }
    g_sink = (uint64_t)x;
}

__attribute__((noinline)) static void chase(const uint32_t *next, long n)
{
    uint32_t p = 0;
    for (long i = 0; i < n; i++) p = next[p];
    g_sink = p;
}

__attribute__((noinline)) static void stream(double *a, const double *b, const double *c,
                                             long n, double s)
{
    for (long i = 0; i < n; i++) a[i] = b[i] + s * c[i];
    __asm__ volatile("" ::: "memory");
}

/* The branch is written in asm: clang (and so AOCC) turns the C version
 * into a cmov, which never mispredicts. */
__attribute__((noinline)) static void branch(const uint8_t *bits, long n)
{
    uint64_t s = 0;
    for (long i = 0; i < n; i++) {
#if defined(__x86_64__)
        uint64_t bit = bits[i], v = (uint64_t)i;
        __asm__ volatile("test $1, %1\n\t"
                         "jz 1f\n\t"
                         "add %2, %0\n\t"
                         "jmp 2f\n"
                         "1:\txor %2, %0\n"
                         "2:"
                         : "+r"(s) : "r"(bit), "r"(v) : "cc");
#else
        if (bits[i] & 1) s += (uint64_t)i; else s ^= (uint64_t)i;
#endif
    }
    g_sink = s;
}

/* Run `body` repeatedly inside region `name` for about TARGET_MS. */
#define TIMED_REGION(name, body)                                    \
    do {                                                            \
        miniprof_region_begin(name);                                \
        double t0_ = now();                                         \
        int reps_ = 0;                                              \
        do { body; reps_++; } while ((now() - t0_) * 1e3 < TARGET_MS); \
        miniprof_region_end(name);                                  \
        if (rank == 0)                                              \
            printf("  %-8s %3d reps  %.0f ms\n", name, reps_,       \
                   (now() - t0_) * 1e3);                            \
    } while (0)

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    double scale = argc > 1 ? atof(argv[1]) : 1.0;
    if (scale <= 0) scale = 1.0;

    /* sizes: chase 128 MiB of indices, stream 3 x 128 MiB — far beyond L3 */
    long nchase  = (long)(32L * 1024 * 1024 * scale);
    long nstream = (long)(16L * 1024 * 1024 * scale);
    long nbranch = (long)(16L * 1024 * 1024 * scale);
    if (nchase < 1024) nchase = 1024;
    if (nstream < 1024) nstream = 1024;
    if (nbranch < 1024) nbranch = 1024;

    uint32_t *next = malloc((size_t)nchase * sizeof(uint32_t));
    double *a = malloc((size_t)nstream * sizeof(double));
    double *b = malloc((size_t)nstream * sizeof(double));
    double *c = malloc((size_t)nstream * sizeof(double));
    uint8_t *bits = malloc((size_t)nbranch);
    if (!next || !a || !b || !c || !bits) { fprintf(stderr, "out of memory\n"); MPI_Abort(MPI_COMM_WORLD, 1); }

    /* one random cycle through all nchase slots (Sattolo's algorithm) */
    uint64_t seed = 0x9e3779b97f4a7c15ull ^ (uint64_t)rank;
    for (long i = 0; i < nchase; i++) next[i] = (uint32_t)i;
    for (long i = nchase - 1; i > 0; i--) {
        long j = (long)(xorshift(&seed) % (uint64_t)i);
        uint32_t t = next[i]; next[i] = next[j]; next[j] = t;
    }
    for (long i = 0; i < nstream; i++) { a[i] = 0; b[i] = 1.0; c[i] = 2.0; }
    for (long i = 0; i < nbranch; i++) bits[i] = (uint8_t)xorshift(&seed);

    if (rank == 0) printf("topdown_validate: %d ranks, scale %g\n", nprocs, scale);
    TIMED_REGION("alu",     alu(50L * 1000 * 1000));
    TIMED_REGION("fpchain", fpchain(20L * 1000 * 1000));
    TIMED_REGION("chase",   chase(next, nchase / 4));
    TIMED_REGION("stream",  stream(a, b, c, nstream, 3.0));
    TIMED_REGION("branch",  branch(bits, nbranch));

    double x = (double)g_sink, sum = 0;
    MPI_Allreduce(&x, &sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    free(next); free(a); free(b); free(c); free(bits);
    MPI_Finalize();
    return 0;
}
