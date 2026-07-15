/*
 * sampler_hwc.c — perf_event_open hardware counter group
 *
 * Opens three counters as a group (CYCLES as leader, INSTRUCTIONS and
 * CACHE_MISSES attached to it) so a single read() returns a consistent
 * snapshot of all three via PERF_FORMAT_GROUP. That single read() is a
 * plain fd read — async-signal-safe — which is what will eventually let
 * the SIGPROF handler sample counters alongside the PC (not wired up
 * yet in this increment; see hwc_read()'s doc comment).
 *
 * This module must never turn a missing/denied counter into a fatal
 * error: perf_event_open requires either CAP_PERFMON or
 * kernel.perf_event_paranoid <= 2, and plenty of HPC login/compute
 * nodes (and every container without --privileged) lock that down.
 * Every failure path here degrades to hwc_enabled() == 0 and prints a
 * one-time diagnostic; sampling itself is entirely unaffected.
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

static int g_fd_cycles = -1;   /* group leader */
static int g_fd_insns  = -1;
static int g_fd_misses = -1;
static int g_enabled   = 0;

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

static void close_all(void)
{
    if (g_fd_insns  >= 0) { close(g_fd_insns);  g_fd_insns  = -1; }
    if (g_fd_misses >= 0) { close(g_fd_misses); g_fd_misses = -1; }
    if (g_fd_cycles >= 0) { close(g_fd_cycles); g_fd_cycles = -1; }
}

int hwc_init(void)
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

    g_fd_cycles = perf_event_open(&attr, -1);
    if (g_fd_cycles < 0) {
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
                    "(perf_event_open: %s). Profiling continues with "
                    "sampling only.\n", strerror(errno));
            }
        }
        g_enabled = 0;
        return 0;
    }

    /* Attach INSTRUCTIONS and CACHE_MISSES to the same group. Group
     * members inherit disabled=0 from the leader's enable state, so we
     * flip attr.disabled off before opening them. */
    attr.disabled = 0;

    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    g_fd_insns  = perf_event_open(&attr, g_fd_cycles);

    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    g_fd_misses = perf_event_open(&attr, g_fd_cycles);

    if (g_fd_insns < 0 || g_fd_misses < 0) {
        if (is_rank0()) {
            fprintf(stderr,
                "[miniprof] partial hardware counter support (%s), "
                "falling back to sampling only.\n", strerror(errno));
        }
        close_all();
        g_enabled = 0;
        return 0;
    }

    if (ioctl(g_fd_cycles, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
        ioctl(g_fd_cycles, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        if (is_rank0()) {
            fprintf(stderr,
                "[miniprof] failed to enable hardware counter group "
                "(%s), falling back to sampling only.\n",
                strerror(errno));
        }
        close_all();
        g_enabled = 0;
        return 0;
    }

    g_enabled = 1;

    /* Self-test: confirm the group actually produces a well-formed
     * snapshot before trusting it for the rest of the run. */
    hwc_snapshot_t snap;
    if (hwc_read(&snap) != 0 || snap.nr != 3) {
        if (is_rank0()) {
            fprintf(stderr,
                "[miniprof] hardware counter group opened but read() "
                "self-test failed, falling back to sampling only.\n");
        }
        close_all();
        g_enabled = 0;
        return 0;
    }

    if (is_rank0() && getenv("MINIPROF_VERBOSE") && atoi(getenv("MINIPROF_VERBOSE"))) {
        fprintf(stderr,
            "[miniprof] hardware counters enabled "
            "(cycles, instructions, cache-misses)\n");
    }

    return 1;
}

int hwc_enabled(void)
{
    return g_enabled;
}

int hwc_read(hwc_snapshot_t *out)
{
    if (!g_enabled || g_fd_cycles < 0) return -1;

    /* One read() on the group leader returns all member counters
     * (PERF_FORMAT_GROUP) plus enabled/running time — a plain fd read,
     * async-signal-safe, which is what makes this safe to eventually
     * call from the SIGPROF handler. Not yet wired in: this increment
     * only establishes and self-tests the counter group. */
    ssize_t n = read(g_fd_cycles, out, sizeof(*out));
    if (n != (ssize_t)sizeof(*out) || out->nr != 3) return -1;
    return 0;
}

void hwc_shutdown(void)
{
    close_all();
    g_enabled = 0;
}
