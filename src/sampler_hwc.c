/*
 * sampler_hwc.c — perf_event_open hardware counter group, per OS thread
 *
 * Opens three counters as a group (CYCLES as leader, INSTRUCTIONS and
 * CACHE_MISSES attached to it) so a single read() returns a consistent
 * snapshot of all three via PERF_FORMAT_GROUP.
 *
 * PER-THREAD, NOT PER-PROCESS: perf_event_open() with pid=0 counts only
 * the calling thread. Sample mode's SIGPROF handler and trace mode's
 * __cyg_profile_func_enter/exit can all fire on whatever OS thread is
 * currently running — the MPI_Init-calling thread for a single-threaded
 * MPI rank, but any OpenMP worker thread too in an MPI+OpenMP hybrid
 * code. An earlier version of this file opened one counter group on
 * whichever thread called hwc_init() and used it from every thread's
 * signal handler unconditionally: reading a group opened by thread A
 * while thread B is what's actually executing returns thread A's
 * counters, not thread B's — thread A being wherever the MPI_Init
 * caller was at the time (often idling at an OpenMP barrier), so every
 * sample regardless of which worker function it landed in reported
 * near-identical numbers dominated by thread A's activity. Confirmed
 * against a real OpenMP+MPI NAS Parallel Benchmark run: Cyc/samp came
 * out within 0.2% of each other across seven structurally unrelated
 * kernels (LU block solve, dense matmul, tridiagonal solves), which is
 * not physically plausible for genuinely distinct compute — the
 * signature of measuring the same thread's counters every time.
 *
 * Fix: `_Thread_local` state below, one counter group per OS thread,
 * lazily opened (silently — no stdio, so it's safe from a signal
 * handler) the first time that thread calls hwc_read()/hwc_read_delta().
 * hwc_init() remains the eager, diagnostic-printing entry point for the
 * *first* thread (the one that calls MPI_Init, or instrument_load_trace_
 * list() in trace mode) — it shares the same underlying open logic, just
 * with a one-time stderr message on failure so paranoid/unsupported
 * systems still get one clear explanation instead of N silent no-ops.
 *
 * This module must never turn a missing/denied counter into a fatal
 * error: perf_event_open requires either CAP_PERFMON or
 * kernel.perf_event_paranoid <= 2, and plenty of HPC login/compute
 * nodes (and every container without --privileged) lock that down.
 * Every failure path here degrades to that thread's counters being
 * unavailable; sampling itself is entirely unaffected.
 */

#define _GNU_SOURCE
#include "sampler_hwc.h"
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <mpi.h>

/* ── per-thread state ─────────────────────────────────────────────────── */
typedef struct {
    int fd_cycles;   /* group leader; -1 = not opened */
    int fd_insns;
    int fd_misses;
    int state;       /* 0 = not attempted yet, 1 = enabled, -1 = failed */
    hwc_snapshot_t prev_delta; /* baseline for hwc_read_delta() on this thread */
} hwc_tls_t;

static _Thread_local hwc_tls_t g_tls = { .fd_cycles = -1, .fd_insns = -1,
                                         .fd_misses = -1, .state = 0 };

/* Set by hwc_init() for the *first* (eager) thread only — this is what
 * hwc_enabled() reports, used for the rank-level "counters available"
 * summary in reports. Other threads open their own groups lazily and
 * independently; a later thread's failure (e.g. running out of fds
 * across hundreds of OpenMP threads) doesn't flip this back off, and a
 * later thread's success doesn't require this to have been 1 first —
 * each thread's actual availability is simply whether *that* thread's
 * hwc_read()/hwc_read_delta() calls succeed. */
static int g_first_thread_enabled = 0;

static long perf_event_open(struct perf_event_attr *attr, int group_fd)
{
    /* pid=0 (calling thread), cpu=-1 (any CPU the thread runs on) */
    return syscall(__NR_perf_event_open, attr, 0, -1, group_fd, 0);
}

/* Best-effort read of /proc/sys/kernel/perf_event_paranoid for the
 * diagnostic message; -1 if it can't be read (non-Linux /proc layout,
 * permissions, etc.) rather than treated as an error. */
static int read_paranoid_level(void)
{
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!f) return -1;
    int level = -1;
    if (fscanf(f, "%d", &level) != 1) level = -1;
    fclose(f);
    return level;
}

/* Diagnostics are rank-0-only: on a denied/unsupported system every
 * rank hits the identical error, and an N-rank job would otherwise
 * flood stderr with N copies of the same line. */
static int is_rank0(void)
{
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (!initialized) return 1;
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return rank == 0;
}

static void tls_close_all(void)
{
    if (g_tls.fd_insns  >= 0) { close(g_tls.fd_insns);  g_tls.fd_insns  = -1; }
    if (g_tls.fd_misses >= 0) { close(g_tls.fd_misses); g_tls.fd_misses = -1; }
    if (g_tls.fd_cycles >= 0) { close(g_tls.fd_cycles); g_tls.fd_cycles = -1; }
}

/*
 * Core open+enable+self-test for the CALLING thread's counter group.
 * No stdio, no MPI calls — every operation here is a raw syscall
 * (perf_event_open/ioctl/read), so this is safe to call the first time
 * any thread (including from inside a signal handler) needs its group.
 * Returns 1 on success, 0 on failure (errno left set to the last
 * failing syscall's error for hwc_init()'s diagnostics to inspect).
 */
static int tls_open_silent(void)
{
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.size           = sizeof(attr);
    attr.disabled       = 1;
    attr.exclude_kernel  = 1;  /* user-space only: avoids needing
                                  CAP_PERFMON on top of a permissive
                                  perf_event_paranoid on some kernels  */
    attr.exclude_hv      = 1;
    attr.type            = PERF_TYPE_HARDWARE;
    attr.config           = PERF_COUNT_HW_CPU_CYCLES;
    attr.read_format      = PERF_FORMAT_GROUP
                           | PERF_FORMAT_TOTAL_TIME_ENABLED
                           | PERF_FORMAT_TOTAL_TIME_RUNNING;

    g_tls.fd_cycles = perf_event_open(&attr, -1);
    if (g_tls.fd_cycles < 0) { g_tls.state = -1; return 0; }

    /* Attach INSTRUCTIONS and CACHE_MISSES to the same group. Group
     * members inherit disabled=0 from the leader's enable state, so we
     * flip attr.disabled off before opening them. */
    attr.disabled = 0;

    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    g_tls.fd_insns = perf_event_open(&attr, g_tls.fd_cycles);

    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    g_tls.fd_misses = perf_event_open(&attr, g_tls.fd_cycles);

    if (g_tls.fd_insns < 0 || g_tls.fd_misses < 0) {
        tls_close_all();
        g_tls.state = -1;
        return 0;
    }

    if (ioctl(g_tls.fd_cycles, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
        ioctl(g_tls.fd_cycles, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        tls_close_all();
        g_tls.state = -1;
        return 0;
    }

    g_tls.state = 1; /* hwc_read() below requires this to already be 1 */

    /* Self-test: confirm the group actually produces a well-formed
     * snapshot before trusting it. Doubles as the baseline for this
     * thread's hwc_read_delta(). */
    if (hwc_read(&g_tls.prev_delta) != 0 || g_tls.prev_delta.nr != 3) {
        tls_close_all();
        g_tls.state = -1;
        return 0;
    }

    return 1;
}

int hwc_init(void)
{
    if (g_tls.state != 0) return g_tls.state == 1; /* already attempted */

    if (tls_open_silent()) {
        g_first_thread_enabled = 1;
        if (is_rank0() && getenv("MINIPROF_VERBOSE") && atoi(getenv("MINIPROF_VERBOSE"))) {
            fprintf(stderr,
                "[miniprof] hardware counters enabled "
                "(cycles, instructions, cache-misses)\n");
        }
        return 1;
    }

    if (is_rank0()) {
        if (errno == EACCES || errno == EPERM) {
            fprintf(stderr,
                "[miniprof] hardware counters unavailable "
                "(perf_event_paranoid=%d, need <=2). Profiling "
                "continues with sampling only.\n"
                "[miniprof] To enable: "
                "sudo sysctl kernel.perf_event_paranoid=2\n",
                read_paranoid_level());
        } else {
            fprintf(stderr,
                "[miniprof] hardware counters unavailable "
                "(%s). Profiling continues with sampling only.\n",
                strerror(errno));
        }
    }
    g_first_thread_enabled = 0;
    return 0;
}

int hwc_enabled(void)
{
    return g_first_thread_enabled;
}

int hwc_read(hwc_snapshot_t *out)
{
    /* Lazily open THIS thread's own group on first use — silent (no
     * stdio/MPI calls in tls_open_silent()), so this is safe to reach
     * for the first time from inside a signal handler on a thread that
     * never explicitly called hwc_init() (e.g. an OpenMP worker). */
    if (g_tls.state == 0) tls_open_silent();
    if (g_tls.state != 1) return -1;

    /* One read() on the group leader returns all member counters
     * (PERF_FORMAT_GROUP) plus enabled/running time — a plain fd read,
     * async-signal-safe. */
    ssize_t n = read(g_tls.fd_cycles, out, sizeof(*out));
    if (n != (ssize_t)sizeof(*out) || out->nr != 3) return -1;
    return 0;
}

void hwc_delta(const hwc_snapshot_t *before, const hwc_snapshot_t *after,
              uint64_t *cycles, uint64_t *instructions,
              uint64_t *cache_misses)
{
    uint64_t dcyc  = after->cycles       - before->cycles;
    uint64_t dins  = after->instructions - before->instructions;
    uint64_t dmiss = after->cache_misses - before->cache_misses;

    /* PMU multiplexing correction: if the kernel time-shared our group
     * with other active counters (e.g. a concurrent `perf stat`), only
     * a fraction of the elapsed time was actually spent counting.
     * Scaling by time_enabled/time_running extrapolates to what the
     * full-time count would have been — the same correction `perf
     * stat` itself applies. */
    uint64_t dt_en  = after->time_enabled - before->time_enabled;
    uint64_t dt_run = after->time_running - before->time_running;
    if (dt_run > 0 && dt_run < dt_en) {
        double scale = (double)dt_en / (double)dt_run;
        dcyc  = (uint64_t)(dcyc  * scale);
        dins  = (uint64_t)(dins  * scale);
        dmiss = (uint64_t)(dmiss * scale);
    }

    *cycles = dcyc; *instructions = dins; *cache_misses = dmiss;
}

int hwc_read_delta(uint64_t *cycles, uint64_t *instructions,
                   uint64_t *cache_misses)
{
    hwc_snapshot_t snap;
    if (hwc_read(&snap) != 0) return -1;

    hwc_delta(&g_tls.prev_delta, &snap, cycles, instructions, cache_misses);
    g_tls.prev_delta = snap;
    return 0;
}

void hwc_shutdown(void)
{
    /* Only closes the CALLING thread's fds — that's the only state
     * accessible from here. Other threads' per-thread groups (e.g.
     * OpenMP workers that never call this) are closed implicitly by
     * the OS when those threads exit or the process terminates, which
     * happens shortly after MPI_Finalize either way. */
    tls_close_all();
    g_tls.state = 0;
    memset(&g_tls.prev_delta, 0, sizeof(g_tls.prev_delta));
    if (g_first_thread_enabled) g_first_thread_enabled = 0;
}
