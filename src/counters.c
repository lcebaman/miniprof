/*
 * counters.c — snapshot-mode hardware counter engine. See counters.h.
 *
 * ── Event tables ────────────────────────────────────────────────────────
 * Encodings are from the Linux kernel's perf event definitions
 * (tools/perf/pmu-events/arch/x86/amdzen4 and amdzen5), which follow the
 * AMD PPRs. Zen 4 and Zen 5 differ where it matters: fp_ret_sse_avx_ops.all
 * is umask 0x1f on Zen 4 but 0x0f on Zen 5 (Zen 5 reuses the high umask
 * bits for a precision field), so each generation has its own table.
 *
 * ── Why groups have an optional tail ────────────────────────────────────
 * Zen exposes 6 general-purpose core counters per thread and no fixed
 * ones; fp_ret_sse_avx_ops (FLOPs) is a large-increment event that takes
 * two of them. When the NMI watchdog is on it permanently occupies one
 * more, so a group that needs all 6 passes perf_event_open's validation
 * but is never scheduled (time_running stays 0, every count reads 0), and
 * one that needs 7 fails to open. cnt_init() tries the full group, then
 * trims optional events from the end — on open failure or if the group
 * never ran — until it fits or only the required events are left.
 *
 * ── rdpmc fast path ─────────────────────────────────────────────────────
 * Each event fd's first page is mmapped; when the kernel sets
 * cap_user_rdpmc and the event is on the PMU (index != 0) the count is
 * offset + sign-extended rdpmc(index - 1), read under the page's seqlock
 * (the documented protocol in linux/perf_event.h). If any event of the
 * group is not currently scheduled, the whole group falls back to one
 * read() — never mixing the two within one snapshot.
 */

#define _GNU_SOURCE
#include "counters.h"
#include "../include/miniprof.h"

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── tables ──────────────────────────────────────────────────────────── */

#define RAW(ev, um) PERF_TYPE_RAW, CNT_AMD_RAW(ev, um)
#define HW(cfg)     PERF_TYPE_HARDWARE, PERF_COUNT_HW_##cfg

static const cnt_group_t k_groups[] = {
    /* base: cycles, instructions, FLOPs required. fp_ret_sse_avx_ops is a
     * large-increment event that occupies TWO counters, so the required
     * part already uses 4 of the 6; the optional tail is ordered by value
     * and trimmed from the end until the group fits (5 counters with the
     * NMI watchdog on, 6 without). Measured on EPYC 9754 / RHEL 9.7:
     * a 6-counter group never schedules while nmi_watchdog=1. */
    { "zen4", "base", 3, 6, {
        { CK_CYCLES,            "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_INSTRUCTIONS,      "ex_ret_instr",                       RAW(0x0c0, 0x00) },
        { CK_FLOPS,             "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x1f) },
        { CK_DRAM_FILLS,        "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_FILLS_REMOTE, "ls_any_fills_from_sys.dram_io_far",  RAW(0x044, 0x40) },
        { CK_L2_MISSES,         "l2_cache_req_stat.ic_dc_miss_in_l2", RAW(0x064, 0x09) },
    } },
    { "zen5", "base", 3, 6, {
        { CK_CYCLES,            "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_INSTRUCTIONS,      "ex_ret_instr",                       RAW(0x0c0, 0x00) },
        { CK_FLOPS,             "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x0f) },
        { CK_DRAM_FILLS,        "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_FILLS_REMOTE, "ls_any_fills_from_sys.dram_io_far",  RAW(0x044, 0x40) },
        { CK_L2_MISSES,         "l2_cache_req_stat.ic_dc_miss_in_l2", RAW(0x064, 0x09) },
    } },
    /* Zen 5 FLOP precision split, for the roofline group (P6). The precision
     * field (umask bits 7:5) only filters: it must be combined with the
     * op-type bits 3:0. Measured on EPYC 9555 (perf 5.14 RHEL 9.7), the
     * four variants below sum exactly to .all (0x0f); the kernel JSON's
     * 0x40/0x60/0x80/0xa0 count nothing:
     *   0x4f scalar single   0x6f packed single
     *   0x8f scalar double   0xaf packed double
     * Each is a two-counter event, so at most two fit in one group with
     * the NMI watchdog on. Zen 4 has no precision field. */

    /* vector: FP ops by width (one counter each) + cycles = 5 counters,
     * fits with the watchdog on. FLOPs (2 counters) would not fit with
     * all four widths; they come from the base group. */
    { "zen4", "vector", 4, 5, {
        { CK_FP_SCALAR, "fp_ops_retired_by_width.scalar_uops_retired",   RAW(0x008, 0x04) },
        { CK_FP_128,    "fp_ops_retired_by_width.pack_128_uops_retired", RAW(0x008, 0x08) },
        { CK_FP_256,    "fp_ops_retired_by_width.pack_256_uops_retired", RAW(0x008, 0x10) },
        { CK_FP_512,    "fp_ops_retired_by_width.pack_512_uops_retired", RAW(0x008, 0x20) },
        { CK_CYCLES,    "ls_not_halted_cyc",                          RAW(0x076, 0x00) },
    } },
    { "zen5", "vector", 4, 5, {
        { CK_FP_SCALAR, "fp_ops_retired_by_width.scalar_uops_retired",   RAW(0x008, 0x04) },
        { CK_FP_128,    "fp_ops_retired_by_width.pack_128_uops_retired", RAW(0x008, 0x08) },
        { CK_FP_256,    "fp_ops_retired_by_width.pack_256_uops_retired", RAW(0x008, 0x10) },
        { CK_FP_512,    "fp_ops_retired_by_width.pack_512_uops_retired", RAW(0x008, 0x20) },
        { CK_CYCLES,    "ls_not_halted_cyc",                          RAW(0x076, 0x00) },
    } },
    /* Test-only (MINIPROF_COUNTERS_TEST=1): software events, so the whole
     * engine — groups, thread slots, pthread hook, MPI boundaries,
     * regions, collect — can be exercised on machines without a PMU
     * (CI, VMs). task-clock (ns on CPU) stands in for cycles. */
    { "swtest", "base", 2, 3, {
        { CK_CYCLES,       "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_INSTRUCTIONS, "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_L2_MISSES,    "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
    } },
    /* anything else (incl. Intel until P8): architectural events only */
    { "generic", "base", 2, 2, {
        { CK_CYCLES,       "cycles",       HW(CPU_CYCLES) },
        { CK_INSTRUCTIONS, "instructions", HW(INSTRUCTIONS) },
    } },
};
#define N_GROUPS (int)(sizeof(k_groups) / sizeof(k_groups[0]))

static const char *k_kind_names[CK_COUNT] = {
    "cycles", "instructions", "flops", "l2_misses", "dram_fills",
    "dram_fills_remote", "fp_scalar_ops", "fp_packed128_ops",
    "fp_packed256_ops", "fp_packed512_ops"
};

const char *cnt_kind_name(cnt_kind_t k)
{
    return (k >= 0 && k < CK_COUNT) ? k_kind_names[k] : "?";
}

const cnt_group_t *cnt_lookup(const char *uarch, const char *group)
{
    for (int i = 0; i < N_GROUPS; i++)
        if (uarch && strcmp(k_groups[i].uarch, uarch) == 0 &&
            strcmp(k_groups[i].group, group) == 0)
            return &k_groups[i];
    for (int i = 0; i < N_GROUPS; i++)
        if (strcmp(k_groups[i].uarch, "generic") == 0) return &k_groups[i];
    return &k_groups[0];   /* unreachable: generic is in the table */
}

/* ── per-thread state ────────────────────────────────────────────────── */

#define CNT_MAX_THREADS 1024

typedef struct {
    int      region;          /* index into g_regions */
    uint64_t t0_ns;
    uint64_t v0[CNT_MAX_EVENTS];
} region_frame_t;

typedef struct {
    int      fd[CNT_MAX_EVENTS];
    struct perf_event_mmap_page *pg[CNT_MAX_EVENTS];
    int      in_mpi;
    uint64_t last[CNT_MAX_EVENTS];
    uint64_t acc[2][CNT_MAX_EVENTS];   /* 0 = compute, 1 = mpi */
    int      depth;
    region_frame_t stack[CNT_REGION_DEPTH];
    uint64_t n_rdpmc, n_syscall;       /* which read path boundaries took */
    pid_t    tid;                      /* kernel thread id the group counts */
} thread_slot_t;

static thread_slot_t g_slots[CNT_MAX_THREADS];
static int  g_nslots = 0;                     /* atomic */
static int  g_dropped = 0;                    /* atomic */
static _Thread_local thread_slot_t *t_slot = NULL;
static _Thread_local int t_open_failed = 0;

static const cnt_group_t *g_group = NULL;
static int  g_nev = 0;           /* events in use (required [+ optional]) */
static int  g_active = 0;        /* counting enabled                      */
static int  g_regions_on = 0;    /* regions record time even w/o counters */
static int  g_use_rdpmc = 0;
static int  g_split = 1;         /* reads at MPI calls (compute/MPI split) */
static char g_reason[160] = "";
static long g_pagesize = 4096;
static thread_slot_t *g_main_slot = NULL;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── perf plumbing ───────────────────────────────────────────────────── */

static long sys_perf_event_open(struct perf_event_attr *a, pid_t pid, int group_fd)
{
    return syscall(__NR_perf_event_open, a, pid /* 0 = this thread */, -1, group_fd, 0);
}

static inline pid_t my_tid(void) { return (pid_t)syscall(SYS_gettid); }

static void close_slot(thread_slot_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s->pg[i]) munmap(s->pg[i], (size_t)g_pagesize);
        if (s->fd[i] >= 0) close(s->fd[i]);
        s->pg[i] = NULL;
        s->fd[i] = -1;
    }
}

/* Open `n` events of the group for the calling thread, disabled; leader
 * first. Returns 0 or -errno. */
static int g_fail_index = -1;   /* index of the event that failed to open */

static int open_events_pid(thread_slot_t *s, int n, pid_t pid)
{
    for (int i = 0; i < CNT_MAX_EVENTS; i++) { s->fd[i] = -1; s->pg[i] = NULL; }
    for (int i = 0; i < n; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof(a));
        a.size           = sizeof(a);
        a.type           = g_group->ev[i].type;
        a.config         = g_group->ev[i].config;
        a.disabled       = (i == 0);       /* members follow the leader */
        a.exclude_kernel = 1;
        a.exclude_hv     = 1;
        a.read_format    = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
        int fd = (int)sys_perf_event_open(&a, pid, i == 0 ? -1 : s->fd[0]);
        if (fd < 0) { int e = errno; close_slot(s, i); g_fail_index = i; return -e; }
        s->fd[i] = fd;
        if (g_use_rdpmc) {
            void *p = mmap(NULL, (size_t)g_pagesize, PROT_READ, MAP_SHARED, fd, 0);
            s->pg[i] = (p == MAP_FAILED) ? NULL : p;
        }
    }
    if (ioctl(s->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
        ioctl(s->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        int e = errno; close_slot(s, n); return -e;
    }
    return 0;
}

static int open_events(thread_slot_t *s, int n) { return open_events_pid(s, n, 0); }

typedef struct {
    uint64_t nr, time_enabled, time_running;
    uint64_t v[CNT_MAX_EVENTS];
} group_read_t;

static int read_group(const thread_slot_t *s, group_read_t *g)
{
    ssize_t want = (ssize_t)((3 + g_nev) * sizeof(uint64_t));
    return read(s->fd[0], g, sizeof(*g)) >= want ? 0 : -1;
}

#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t rdpmc(uint32_t c)
{
    uint32_t lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(c));
    return ((uint64_t)hi << 32) | lo;
}
#define HAVE_RDPMC 1
#else
#define HAVE_RDPMC 0
#endif

/* One event via its mmap page; 0 on success, -1 if not on the PMU. */
static inline int rdpmc_one(struct perf_event_mmap_page *pc, uint64_t *out)
{
#if HAVE_RDPMC
    uint32_t seq, idx;
    uint64_t count;
    do {
        seq = pc->lock;
        __asm__ volatile("" ::: "memory");
        idx = pc->index;
        count = (uint64_t)pc->offset;
        if (!pc->cap_user_rdpmc || idx == 0) return -1;
        uint64_t pmc = rdpmc(idx - 1);
        int w = pc->pmc_width;
        pmc <<= 64 - w;
        pmc = (uint64_t)((int64_t)pmc >> (64 - w));   /* sign-extend */
        count += pmc;
        __asm__ volatile("" ::: "memory");
    } while (pc->lock != seq);
    *out = count;
    return 0;
#else
    (void)pc; (void)out;
    return -1;
#endif
}

/* Current counts of the calling thread's group. */
static int read_now(thread_slot_t *s, uint64_t *v)
{
    if (g_use_rdpmc) {
        int ok = 1;
        for (int i = 0; i < g_nev && ok; i++)
            ok = s->pg[i] && rdpmc_one(s->pg[i], &v[i]) == 0;
        if (ok) { s->n_rdpmc++; return 0; }
    }
    group_read_t g;
    if (read_group(s, &g) != 0) return -1;
    s->n_syscall++;
    for (int i = 0; i < g_nev; i++) v[i] = g.v[i];
    return 0;
}

/* ── thread slots ────────────────────────────────────────────────────── */

/* slot already opened for this kernel thread (attached at init)? */
static thread_slot_t *slot_by_tid(pid_t tid)
{
    int n = __atomic_load_n(&g_nslots, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_THREADS) n = CNT_MAX_THREADS;
    for (int i = 0; i < n; i++)
        if (g_slots[i].tid == tid && g_slots[i].fd[0] >= 0) return &g_slots[i];
    return NULL;
}

static thread_slot_t *thread_open(void)
{
    if (t_slot) return t_slot;
    if (!g_active || t_open_failed) return NULL;
    pid_t tid = my_tid();
    thread_slot_t *found = slot_by_tid(tid);
    if (found) { t_slot = found; return found; }
    int idx = __atomic_fetch_add(&g_nslots, 1, __ATOMIC_ACQ_REL);
    if (idx >= CNT_MAX_THREADS) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        t_open_failed = 1;
        return NULL;
    }
    thread_slot_t *s = &g_slots[idx];
    memset(s, 0, sizeof(*s));
    if (open_events(s, g_nev) != 0) {    /* leaves s->fd[] = -1 */
        /* keep the slot (fds = -1) so collect() skips it */
        t_open_failed = 1;
        return NULL;
    }
    s->tid = tid;
    read_now(s, s->last);
    t_slot = s;
    return s;
}

/* ── threads that started before counters (OpenMP pools) ─────────────── */

#define CNT_MAX_PENDING 1024
static pid_t g_pending[CNT_MAX_PENDING];
static int   g_npending = 0;

void cnt_thread_hook(void)
{
    if (g_active) { thread_open(); return; }
    int i = __atomic_fetch_add(&g_npending, 1, __ATOMIC_ACQ_REL);
    if (i < CNT_MAX_PENDING) g_pending[i] = my_tid();
}

/* Open groups for remembered threads from the init thread (perf allows
 * counting another thread of the same process). Their reads at region
 * boundaries still use rdpmc from their own thread, via slot_by_tid. */
static void attach_pending(void)
{
    int n = __atomic_load_n(&g_npending, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_PENDING) n = CNT_MAX_PENDING;
    pid_t self = my_tid();
    for (int i = 0; i < n; i++) {
        pid_t tid = g_pending[i];
        if (tid == self || slot_by_tid(tid)) continue;
        int idx = __atomic_fetch_add(&g_nslots, 1, __ATOMIC_ACQ_REL);
        if (idx >= CNT_MAX_THREADS) { __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED); return; }
        thread_slot_t *s = &g_slots[idx];
        memset(s, 0, sizeof(*s));
        if (open_events_pid(s, g_nev, tid) != 0) continue;   /* thread gone: skip */
        s->tid = tid;
        group_read_t g;
        if (read_group(s, &g) == 0)
            for (int e = 0; e < g_nev; e++) s->last[e] = g.v[e];
    }
}

/* ── pthread_create interposer: threads started after init count from
 *    their first instruction ─────────────────────────────────────────── */

typedef struct { void *(*fn)(void *); void *arg; } tramp_t;

static void *trampoline(void *p)
{
    tramp_t t = *(tramp_t *)p;
    free(p);
    thread_open();
    return t.fn(t.arg);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "pthread_create");
    if (!g_active) return real(th, attr, fn, arg);
    tramp_t *t = malloc(sizeof(*t));
    if (!t) return real(th, attr, fn, arg);
    t->fn = fn;
    t->arg = arg;
    int rc = real(th, attr, trampoline, t);
    if (rc != 0) free(t);
    return rc;
}

/* ── init / stop ─────────────────────────────────────────────────────── */

static void spin_us(int us)
{
    uint64_t end = now_ns() + (uint64_t)us * 1000;
    volatile uint64_t x = 0;
    while (now_ns() < end) x++;
}

static int read_int_file(const char *path, int def)
{
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

int cnt_init(const char *uarch, const char *group)
{
    g_regions_on = 1;
    g_pagesize = sysconf(_SC_PAGESIZE);
    g_group = getenv("MINIPROF_COUNTERS_TEST") ? cnt_lookup("swtest", "base")
                                                : cnt_lookup(uarch, group);
    g_nev = g_group->n_events;
    g_use_rdpmc = HAVE_RDPMC &&
                  read_int_file("/sys/bus/event_source/devices/cpu/rdpmc", 1) != 0 &&
                  !getenv("MINIPROF_NO_RDPMC");

    /* Probe on the calling (MPI_Init) thread: the full group first, then
     * trim optional events from the end until the group opens AND gets
     * scheduled. A required event failing to open is fatal (permissions,
     * no PMU); an optional one failing just means "does not fit". */
    g_active = 1;
    int fitted = 0, last_err = 0;
    for (int n = g_group->n_events; n >= g_group->n_required; n--) {
        g_nev = n;
        thread_slot_t probe;
        memset(&probe, 0, sizeof(probe));
        g_fail_index = -1;
        int rc = open_events(&probe, n);
        if (rc != 0) {
            last_err = -rc;
            if (g_fail_index >= 0 && g_fail_index < g_group->n_required) break;
            continue;                         /* optional event did not fit */
        }
        spin_us(2000);
        group_read_t g;
        int ok = read_group(&probe, &g) == 0 && g.time_running > 0;
        close_slot(&probe, n);
        if (ok) { fitted = 1; break; }
        last_err = 0;                         /* opened but never scheduled */
    }
    if (!fitted) {
        g_active = 0;
        if (last_err)
            snprintf(g_reason, sizeof(g_reason),
                     "perf_event_open failed for %s/%s: %s%s", g_group->uarch,
                     g_group->group, strerror(last_err),
                     (last_err == EACCES || last_err == EPERM)
                         ? " (check /proc/sys/kernel/perf_event_paranoid)" :
                     (last_err == ENOENT || last_err == ENODEV || last_err == EOPNOTSUPP)
                         ? " (no PMU: virtual machine or unsupported CPU?)" : "");
        else
            snprintf(g_reason, sizeof(g_reason),
                     "%s/%s: even the %d required events were never scheduled on the "
                     "PMU: counters in use (NMI watchdog, another profiler)?",
                     g_group->uarch, g_group->group, g_group->n_required);
        return 0;
    }

    g_main_slot = thread_open();
    if (!g_main_slot) {
        g_active = 0;
        snprintf(g_reason, sizeof(g_reason), "could not open counters on the main thread");
        return 0;
    }
    attach_pending();
    const char *split = getenv("MINIPROF_COUNTERS_SPLIT");
    g_split = !(split && strcmp(split, "0") == 0);
    return 1;
}

void cnt_stop(void)
{
    g_regions_on = 0;
    /* g_active stays set until collect: boundary reads become no-ops via
     * the mpi_trace mode switch, and threads still running keep counting
     * so the final read sees their full totals. */
}

/* ── MPI boundaries ──────────────────────────────────────────────────── */

static inline void boundary(int entering_mpi)
{
    thread_slot_t *s = t_slot ? t_slot : thread_open();
    if (!s) return;
    uint64_t v[CNT_MAX_EVENTS];
    if (read_now(s, v) != 0) return;
    int bucket = s->in_mpi;
    for (int i = 0; i < g_nev; i++) {
        s->acc[bucket][i] += v[i] - s->last[i];
        s->last[i] = v[i];
    }
    s->in_mpi = entering_mpi;
}

void cnt_mpi_enter(void) { if (g_active && g_split) boundary(1); }
void cnt_mpi_exit(void)  { if (g_active && g_split) boundary(0); }

int cnt_split_enabled(void) { return g_split; }

/* ── regions ─────────────────────────────────────────────────────────── */

static cnt_region_t g_regions[CNT_MAX_REGIONS];
static int g_nregions = 0;
static pthread_mutex_t g_region_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_region_warned = 0;

static int region_id(const char *name)
{
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (strncmp(g_regions[i].name, name, CNT_REGION_NAME - 1) == 0) return i;
    pthread_mutex_lock(&g_region_lock);
    n = g_nregions;
    for (int i = 0; i < n; i++)
        if (strncmp(g_regions[i].name, name, CNT_REGION_NAME - 1) == 0) {
            pthread_mutex_unlock(&g_region_lock);
            return i;
        }
    int id = -1;
    if (n < CNT_MAX_REGIONS) {
        snprintf(g_regions[n].name, CNT_REGION_NAME, "%s", name);
        id = n;
        __atomic_store_n(&g_nregions, n + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_region_lock);
    return id;
}

/* Per-thread stack for threads that have no counter slot (counters
 * unavailable): regions still get calls and time. */
static _Thread_local region_frame_t t_nostack[CNT_REGION_DEPTH];
static _Thread_local int t_nodepth = 0;

void miniprof_region_begin(const char *name)
{
    if (!g_regions_on || !name) return;
    int id = region_id(name);
    if (id < 0) return;
    thread_slot_t *s = g_active ? (t_slot ? t_slot : thread_open()) : NULL;
    region_frame_t *f;
    if (s) {
        if (s->depth >= CNT_REGION_DEPTH) return;
        f = &s->stack[s->depth++];
        if (read_now(s, f->v0) != 0) memset(f->v0, 0, sizeof(f->v0));
    } else {
        if (t_nodepth >= CNT_REGION_DEPTH) return;
        f = &t_nostack[t_nodepth++];
    }
    f->region = id;
    f->t0_ns = now_ns();
}

void miniprof_region_end(const char *name)
{
    if (!g_regions_on) return;
    uint64_t t1 = now_ns();
    thread_slot_t *s = g_active ? t_slot : NULL;
    region_frame_t *f;
    uint64_t v[CNT_MAX_EVENTS];
    int have_v = 0;
    if (s) {
        if (s->depth <= 0) return;
        f = &s->stack[--s->depth];
        have_v = read_now(s, v) == 0;
    } else {
        if (t_nodepth <= 0) return;
        f = &t_nostack[--t_nodepth];
    }
    cnt_region_t *r = &g_regions[f->region];
    if (name && strncmp(r->name, name, CNT_REGION_NAME - 1) != 0 &&
        !__atomic_exchange_n(&g_region_warned, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "[miniprof] region_end(\"%s\") closes \"%s\": "
                        "regions must nest; using the innermost\n", name, r->name);
    __atomic_fetch_add(&r->calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&r->ns, t1 - f->t0_ns, __ATOMIC_RELAXED);
    if (have_v)
        for (int i = 0; i < g_nev; i++)
            __atomic_fetch_add(&r->ev[i], v[i] - f->v0[i], __ATOMIC_RELAXED);
}

/* Fortran: CALL MINIPROF_REGION_BEGIN('name') — hidden length, blank-padded */
static void f_name(char *dst, const char *src, size_t len)
{
    while (len > 0 && src[len - 1] == ' ') len--;
    if (len >= CNT_REGION_NAME) len = CNT_REGION_NAME - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}
void miniprof_region_begin_(const char *name, size_t len)
{
    char buf[CNT_REGION_NAME];
    f_name(buf, name, len);
    miniprof_region_begin(buf);
}
void miniprof_region_end_(const char *name, size_t len)
{
    char buf[CNT_REGION_NAME];
    f_name(buf, name, len);
    miniprof_region_end(buf);
}

int cnt_regions_get(cnt_region_t *out, int max)
{
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].name, g_regions[i].name, CNT_REGION_NAME);
        out[i].calls = __atomic_load_n(&g_regions[i].calls, __ATOMIC_RELAXED);
        out[i].ns    = __atomic_load_n(&g_regions[i].ns, __ATOMIC_RELAXED);
        for (int e = 0; e < CNT_MAX_EVENTS; e++)
            out[i].ev[e] = __atomic_load_n(&g_regions[i].ev[e], __ATOMIC_RELAXED);
    }
    return n;
}

/* ── collect ─────────────────────────────────────────────────────────── */

void cnt_collect(cnt_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->group = g_group;
    out->rdpmc = g_use_rdpmc;
    out->threads_dropped = __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
    if (!g_active) {
        out->available = 0;
        snprintf(out->reason, sizeof(out->reason), "%s",
                 g_reason[0] ? g_reason : "counters not initialised");
        return;
    }
    out->available = 1;
    out->n_active = g_nev;

    int n = __atomic_load_n(&g_nslots, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_THREADS) n = CNT_MAX_THREADS;
    for (int t = 0; t < n; t++) {
        thread_slot_t *s = &g_slots[t];
        if (s->fd[0] < 0) continue;
        /* Final values via read(): works for threads that already exited
         * (the event keeps its final count) and for other threads (rdpmc
         * would read THIS thread's counters). */
        group_read_t g;
        if (read_group(s, &g) != 0) continue;
        if (g.time_running < g.time_enabled - g.time_enabled / 100) out->multiplexed = 1;
        out->threads++;
        out->reads_rdpmc   += s->n_rdpmc;
        out->reads_syscall += s->n_syscall;
        for (int i = 0; i < g_nev; i++) {
            uint64_t tail = g.v[i] - s->last[i];
            uint64_t comp = s->acc[0][i] + (s->in_mpi ? 0 : tail);
            uint64_t mpi  = s->acc[1][i] + (s->in_mpi ? tail : 0);
            out->compute[i] += comp;
            out->mpi[i]     += mpi;
            out->total[i]   += comp + mpi;
            if (s == g_main_slot) out->main_thread[i] = comp + mpi;
        }
    }
    g_active = 0;   /* no more boundary reads */
}
