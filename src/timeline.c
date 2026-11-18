/*
 * timeline.c — snapshot-mode time series per rank. See timeline.h.
 *
 * Concurrency: tl_add may run on any thread (MPI_THREAD_MULTIPLE, I/O
 * from OpenMP threads); additions are relaxed atomics into the bucket of
 * the call's time. Doubling the width (merging buckets) takes a mutex; a
 * call added by another thread while buckets merge can land in the
 * neighbouring bucket. The totals are unaffected; the timeline is a
 * statistical picture at bucket resolution.
 */

#define _GNU_SOURCE
#include "timeline.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int tl_on = 0;
static tl_data_t g;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void tl_init(void)
{
    const char *e = getenv("MINIPROF_TIMELINE");
    if (!e || strcmp(e, "1") != 0) return;
    memset(&g, 0, sizeof(g));
    long ms = 100;
    const char *iv = getenv("MINIPROF_TIMELINE_INTERVAL_MS");
    if (iv) ms = atol(iv);
    if (ms < 1) ms = 1;
    if (ms > 60000) ms = 60000;
    g.enabled = 1;
    g.width_ns = (uint64_t)ms * 1000000ull;
    g.t0_ns = now_ns();
    g.n = 1;
    __atomic_store_n(&tl_on, 1, __ATOMIC_RELEASE);
}

/* merge neighbouring buckets until bucket `idx` fits (lock held) */
static void grow_locked(uint64_t t)
{
    while ((t - g.t0_ns) / g.width_ns >= TL_BUCKETS) {
        for (int i = 0; i < TL_BUCKETS / 2; i++) {
            int a = 2 * i, b = 2 * i + 1;
            g.mpi_ns[i] = g.mpi_ns[a] + g.mpi_ns[b];
            g.io_ns[i]  = g.io_ns[a] + g.io_ns[b];
            int m = g.mark_valid[a] ? a : b;    /* keep the earlier mark */
            g.mark_valid[i] = g.mark_valid[m];
            g.mark_t[i] = g.mark_t[m];
            memcpy(g.mark[i], g.mark[m], sizeof(g.mark[i]));
        }
        memset(&g.mpi_ns[TL_BUCKETS / 2], 0, sizeof(uint64_t) * TL_BUCKETS / 2);
        memset(&g.io_ns[TL_BUCKETS / 2], 0, sizeof(uint64_t) * TL_BUCKETS / 2);
        memset(&g.mark_valid[TL_BUCKETS / 2], 0, sizeof(unsigned) * TL_BUCKETS / 2);
        g.width_ns *= 2;
        g.n = (g.n + 1) / 2;
    }
}

/* bucket of time t, growing the series if needed; -1 before the start */
static int bucket(uint64_t t)
{
    if (t < g.t0_ns) return -1;
    uint64_t idx = (t - g.t0_ns) / __atomic_load_n(&g.width_ns, __ATOMIC_ACQUIRE);
    if (idx >= TL_BUCKETS) {
        pthread_mutex_lock(&g_lock);
        grow_locked(t);
        pthread_mutex_unlock(&g_lock);
        idx = (t - g.t0_ns) / g.width_ns;
        if (idx >= TL_BUCKETS) idx = TL_BUCKETS - 1;
    }
    int n = __atomic_load_n(&g.n, __ATOMIC_RELAXED);
    while ((int)idx + 1 > n &&
           !__atomic_compare_exchange_n(&g.n, &n, (int)idx + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    return (int)idx;
}

void tl_add(int kind, uint64_t t_end_ns, uint64_t dt_ns)
{
    if (!tl_on) return;
    uint64_t *a = kind == TL_MPI ? g.mpi_ns : g.io_ns;
    uint64_t t0 = t_end_ns - dt_ns;
    if (t0 < g.t0_ns) t0 = g.t0_ns;
    /* split the call over the buckets it spans */
    while (t0 < t_end_ns) {
        int b = bucket(t0);
        if (b < 0) return;
        uint64_t w = g.width_ns;
        uint64_t edge = g.t0_ns + (uint64_t)(b + 1) * w;
        uint64_t piece = (t_end_ns < edge ? t_end_ns : edge) - t0;
        if (b == TL_BUCKETS - 1) piece = t_end_ns - t0;   /* clamp the tail */
        __atomic_fetch_add(&a[b], piece, __ATOMIC_RELAXED);
        t0 += piece;
    }
}

void tl_mark(uint64_t t_ns, const uint64_t cnt[TL_CNT], unsigned valid)
{
    if (!tl_on || !valid) return;
    if (t_ns < g.t0_ns) t_ns = g.t0_ns;
    if (g.t1_ns && t_ns > g.t1_ns) t_ns = g.t1_ns;   /* final read, after the stop */
    int b = bucket(t_ns);
    if (b < 0) return;
    /* the mark is per rank's MPI thread: one writer, no atomics needed
     * beyond publishing valid last */
    g.end_t = t_ns;
    memcpy(g.end_mark, cnt, sizeof(g.end_mark));
    g.end_valid = valid;
    if (g.mark_valid[b]) return;
    g.mark_t[b] = t_ns;
    memcpy(g.mark[b], cnt, sizeof(g.mark[b]));
    __atomic_store_n(&g.mark_valid[b], valid, __ATOMIC_RELEASE);
}

void tl_stop(void)
{
    if (!tl_on) return;
    g.t1_ns = now_ns();
    bucket(g.t1_ns);
}

void tl_get(tl_data_t *out)
{
    memcpy(out, &g, sizeof(*out));
    if (!out->t1_ns) out->t1_ns = now_ns();
}

void tl_rebin(tl_data_t *d, uint64_t width_ns)
{
    while (d->width_ns < width_ns) {
        for (int i = 0; i < TL_BUCKETS / 2; i++) {
            int a = 2 * i, b = 2 * i + 1;
            d->mpi_ns[i] = d->mpi_ns[a] + d->mpi_ns[b];
            d->io_ns[i]  = d->io_ns[a] + d->io_ns[b];
            int m = d->mark_valid[a] ? a : b;
            d->mark_valid[i] = d->mark_valid[m];
            d->mark_t[i] = d->mark_t[m];
            memcpy(d->mark[i], d->mark[m], sizeof(d->mark[i]));
        }
        memset(&d->mpi_ns[TL_BUCKETS / 2], 0, sizeof(uint64_t) * TL_BUCKETS / 2);
        memset(&d->io_ns[TL_BUCKETS / 2], 0, sizeof(uint64_t) * TL_BUCKETS / 2);
        memset(&d->mark_valid[TL_BUCKETS / 2], 0, sizeof(unsigned) * TL_BUCKETS / 2);
        d->width_ns *= 2;
        d->n = (d->n + 1) / 2;
    }
}
