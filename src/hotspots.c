/* hotspots.c — see hotspots.h */
#define _GNU_SOURCE
#include "hotspots.h"
#include "profile_store.h"
#include "symbolizer.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <time.h>

static double hs_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int g_on = 0;
static int g_interval = 10;

int hotspots_requested(void)
{
    const char *e = getenv("MINIPROF_HOTSPOTS");
    return e && *e && strcmp(e, "0") != 0;
}

int hotspots_interval_ms(void) { return g_interval; }

static void on_sigprof(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    profile_store_record((void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
}

void hotspots_start(void)
{
    if (g_on || !hotspots_requested()) return;
    const char *e = getenv("MINIPROF_HOTSPOTS_INTERVAL_MS");
    int ms = e ? atoi(e) : 10;
    g_interval = ms < 1 ? 1 : ms > 1000 ? 1000 : ms;
    profile_store_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return;
    struct itimerval it;
    it.it_interval.tv_sec = g_interval / 1000;
    it.it_interval.tv_usec = (g_interval % 1000) * 1000;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) {
        signal(SIGPROF, SIG_DFL);
        return;
    }
    g_on = 1;
}

void hotspots_stop(void)
{
    if (!g_on) return;
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, NULL);
    signal(SIGPROF, SIG_IGN);           /* a signal already in flight is harmless */
    g_on = 0;
}

/* ── collection ── */
typedef struct { uintptr_t pc; long n; } pcn_t;
static pcn_t *g_pcs; static int g_npc, g_cap;

static int take(uintptr_t pc, long count, uint64_t c, uint64_t i, uint64_t m, void *u)
{
    (void)c; (void)i; (void)m; (void)u;
    if (g_npc == g_cap) {
        int nc = g_cap ? 2 * g_cap : 1024;
        pcn_t *p = realloc(g_pcs, (size_t)nc * sizeof(*p));
        if (!p) return 1;
        g_pcs = p; g_cap = nc;
    }
    g_pcs[g_npc++] = (pcn_t){ pc, count };
    return 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const miniprof_sym_t *)a)->name, ((const miniprof_sym_t *)b)->name);
}

static int by_samples(const void *a, const void *b)
{
    long x = ((const hs_entry_t *)a)->samples, y = ((const hs_entry_t *)b)->samples;
    return (y > x) - (y < x);
}

int hotspots_collect(hs_entry_t *out, int max, long *total)
{
    *total = 0;
    g_npc = 0;
    profile_store_iterate(take, NULL);
    if (g_npc == 0) return 0;
    miniprof_sym_t *s = calloc((size_t)g_npc, sizeof(*s));
    if (!s) return 0;
    for (int i = 0; i < g_npc; i++) { s[i].pc = (void *)g_pcs[i].pc; s[i].count = g_pcs[i].n; }
    double t0 = hs_now();
    symbolizer_resolve(s, g_npc, /*use_addr2line=*/0);   /* names only, in-process */
    double t1 = hs_now();

    /* merge PCs into functions: sort by (name, dso), then fold runs.
     * Unnamed code (stripped libraries) is kept per DSO as "[dso]". */
    for (int i = 0; i < g_npc; i++)
        if (strcmp(s[i].name, "??") == 0 || !s[i].name[0]) {
            const char *d = strrchr(s[i].dso, '/');
            snprintf(s[i].name, sizeof(s[i].name), "[%s]", d ? d + 1 : (s[i].dso[0] ? s[i].dso : "unknown"));
        }
    qsort(s, (size_t)g_npc, sizeof(*s), by_name);
    hs_entry_t *f = calloc((size_t)g_npc, sizeof(*f));
    int nf = 0;
    for (int i = 0; i < g_npc; i++) {
        *total += s[i].count;
        if (nf == 0 || strcmp(f[nf - 1].name, s[i].name) != 0) {
            snprintf(f[nf].name, sizeof(f[nf].name), "%s", s[i].name);
            snprintf(f[nf].group, sizeof(f[nf].group), "%s", s[i].group);
            const char *d = strrchr(s[i].dso, '/');
            snprintf(f[nf].dso, sizeof(f[nf].dso), "%s", d ? d + 1 : s[i].dso);
            nf++;
        }
        f[nf - 1].samples += s[i].count;
    }
    qsort(f, (size_t)nf, sizeof(*f), by_samples);
    const char *v = getenv("MINIPROF_VERBOSE");
    if (v && atoi(v))
        fprintf(stderr, "[miniprof] hotspots: %d addresses -> %d functions; names %.1f ms, merge %.1f ms\n",
                g_npc, nf, 1e3 * (t1 - t0), 1e3 * (hs_now() - t1));
    int n = nf < max ? nf : max;
    memcpy(out, f, (size_t)n * sizeof(*f));
    free(f); free(s);
    return n;
}
