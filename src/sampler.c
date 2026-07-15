/*
 * sampler.c — SIGPROF-based statistical sampler
 *
 * On each SIGPROF:
 *   1. Unwind up to g_depth frames with backtrace()
 *   2. Record the LEAF frame (pcs[0]) in profile_store — this is
 *      the flat/exclusive profile
 *   3. Record the full call chain in path_store — this preserves
 *      caller→callee structure for call-path attribution
 *
 * Only the leaf frame contributes to the flat profile. Recording every
 * frame in the chain was a previous bug that inflated the sample count
 * by ~depth× and diluted real hot-spot percentages to <2% (e.g. a
 * function consuming 13% of real CPU time showed as 1.1% because the
 * signal trampoline, __libc_start_main, _start, main, etc. each got
 * one hit per delivery too, bloating the denominator by 12×).
 */

#define _GNU_SOURCE
#include "sampler.h"
#include "sampler_hwc.h"
#include "profile_store.h"
#include "path_store.h"
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>   /* ucontext_t, REG_RIP             */
#include <execinfo.h>   /* backtrace()                      */
#include <string.h>
#include <stdio.h>

static int g_depth    = 8;
static int g_running  = 0;

/* ── signal handler ──────────────────────────────────────────────────────── */
static void sigprof_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;

    /*
     * The TRUE leaf PC — the instruction the CPU was executing when
     * SIGPROF fired — is NOT pcs[0] from backtrace(). backtrace()
     * called inside a signal handler starts from the handler's own
     * frame, so pcs[0] is always sigprof_handler itself. The real
     * interrupted PC is in the signal context's machine-state register:
     *   ucontext_t → uc_mcontext → gregs[REG_RIP]   (x86-64)
     *
     * We extract it directly from the ucontext passed as the third
     * argument to a SA_SIGINFO handler. This is the same mechanism
     * used by gprof, perf, and every production sampling profiler —
     * backtrace() is only useful for the *caller chain* above the
     * interrupted point, not the interrupted point itself.
     */
    ucontext_t *uc = (ucontext_t *)ctx;
    void *leaf_pc = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];

    /* Flat profile: record ONLY the true leaf (exclusive time).
     * Total samples == number of SIGPROF deliveries, and each
     * function's Samp% is its real fraction of CPU time. */
    profile_store_record(leaf_pc);

    /* Full call chain for call-path attribution.
     * backtrace() gives us the chain from sigprof_handler upward;
     * we prepend the true leaf PC so the path is:
     *   [0] = interrupted instruction (leaf)
     *   [1] = sigprof_handler (this function)
     *   [2] = signal trampoline
     *   [3+] = real caller chain of the interrupted code
     * The path_store consumer will need to skip [1] and [2] when
     * building the calltree, but having the complete raw chain is
     * more robust than trying to splice here. */
    void *chain[MINIPROF_MAX_DEPTH + 1];
    chain[0] = leaf_pc;
    int n = backtrace(chain + 1, g_depth < MINIPROF_MAX_DEPTH
                                  ? g_depth : MINIPROF_MAX_DEPTH);
    path_store_record(chain, n + 1);
}

/* ── public API ──────────────────────────────────────────────────────────── */
void sampler_start(int interval_ms, int depth)
{
    if (g_running) return;
    g_depth   = (depth > 0 && depth <= MINIPROF_MAX_DEPTH) ? depth : 8;
    g_running = 1;

    profile_store_init();
    path_store_init();

    /* Hardware counter group (cycles/instructions/cache-misses). Not
     * yet read from the signal handler — this only establishes and
     * self-tests the counter group so a future increment can start
     * accumulating IPC/cache-miss data per sample. Falls back silently
     * to sampling-only if counters are unavailable (see sampler_hwc.c). */
    hwc_init();

    /* Install signal handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigprof_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        perror("miniprof: sigaction(SIGPROF)");
        return;
    }

    /* Arm the per-process interval timer */
    struct itimerval itv;
    itv.it_interval.tv_sec  = interval_ms / 1000;
    itv.it_interval.tv_usec = (interval_ms % 1000) * 1000;
    itv.it_value             = itv.it_interval;   /* arm immediately */
    if (setitimer(ITIMER_PROF, &itv, NULL) != 0) {
        perror("miniprof: setitimer(ITIMER_PROF)");
    }
}

void sampler_stop(void)
{
    if (!g_running) return;
    g_running = 0;

    /* Disarm timer */
    struct itimerval itv;
    memset(&itv, 0, sizeof(itv));
    setitimer(ITIMER_PROF, &itv, NULL);

    /* Restore default handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);

    hwc_shutdown();
}
