/*
 * ompt.c — OMPT tool: OpenMP serial/parallel time, barrier waits, lock
 * waits and per-region imbalance for snapshot mode. See ompt.h.
 *
 * What each callback records (all times on the calling thread's clock):
 *
 *   parallel_begin/end     encountering thread: region wall time; for the
 *                          outermost level on the initial thread, also the
 *                          rank's "time in parallel regions" (serial = the
 *                          rest of the run)
 *   implicit_task          each team member: its time in the region
 *   sync_region_wait       barrier waits (explicit, implicit at the end of
 *                          worksharing and parallel regions) and task waits
 *   mutex_acquire/acquired time waiting for locks and critical sections
 *
 * Per thread: work = implicit-task time - barrier wait. Per region:
 * imbalance = barrier wait / team time — the share of thread time spent
 * waiting at barriers for the slowest member, APS's "OpenMP imbalance".
 *
 * Callbacks are re-entrant per thread (thread-local stacks, depth 8 for
 * nested parallelism) and lock-free except the first sighting of a new
 * region (mutex, at most OMP_MAX_REGIONS times per run).
 *
 * Counter coverage (counters.c): thread_begin hands each OpenMP thread to
 * the counter engine, which opens its group immediately or, for threads
 * created before MPI_Init, attaches to them when counters start.
 */

#define _GNU_SOURCE
#include "ompt.h"
#include "ompt_abi.h"
#include "counters.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static int  g_started = 0;        /* runtime called our initialize()      */
static int  g_debug = 0;          /* MINIPROF_OMPT_DEBUG=1: trace callbacks */
static int  g_recording = 0;
static char g_runtime[96] = "";
static mp_ompt_set_callback_t g_set_cb = NULL;

static omp_region_t g_regions[OMP_MAX_REGIONS];
static int g_nregions = 0;

/* Region instances and their release times. LLVM libomp ends a worker's
 * wait at the end-of-region (join) barrier — and its implicit task — only
 * when the worker is released for the NEXT parallel region (or at
 * shutdown), so those end times include the serial gap that follows.
 * The encountering thread stamps each instance's end at parallel_end, and
 * a worker event that ends later, for the same instance, is clamped to it.
 * Measured on libomp 18: without the clamp the last region of a run read
 * 50% imbalance instead of the injected 25%. */
static uint64_t g_inst[OMP_MAX_REGIONS];       /* instances started      */
static uint64_t g_rel_inst[OMP_MAX_REGIONS];   /* last instance finished */
static uint64_t g_rel_ns[OMP_MAX_REGIONS];     /* ... and when           */
static pthread_mutex_t g_region_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint64_t work_ns, wait_ns, lock_ns, taskwait_ns;
    int      type;
    /* The thread's outermost implicit task, visible to other threads so
     * ompt_snapshot_end() can account a worker still parked at the join
     * barrier of the run's last region — libomp only reports that wait's
     * end at shutdown, after the snapshot is written. Whoever clears
     * `live` (the worker at task end, or the flush) accounts the task. */
    int      live, in_wait, slot;
    uint64_t t0, inst, wait, wait_t0;
} omp_thread_t;
static omp_thread_t g_threads[OMP_MAX_THREADS];
static int g_nthreads = 0;
static uint64_t g_outer_parallel_ns = 0;

#define DEPTH 8
typedef struct {
    omp_thread_t *thr;
    int      pdepth;                   /* parallel regions this thread encountered */
    uint64_t par_t0[DEPTH];
    int      par_slot[DEPTH];
    uint64_t par_inst[DEPTH];
    int      tdepth;                   /* implicit tasks this thread executes     */
    uint64_t task_t0[DEPTH];
    int      task_slot[DEPTH];
    uint64_t task_inst[DEPTH];
    uint64_t task_wait[DEPTH];
    uint64_t wait_t0, lock_t0;
    int      wait_depth;               /* nested sync_region_wait begin/end */
    int      wait_is_barrier;
} tls_t;
static _Thread_local tls_t t;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define ADD(f, v) __atomic_fetch_add(&(f), (uint64_t)(v), __ATOMIC_RELAXED)
static inline void atomic_max(uint64_t *p, uint64_t v)
{
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(p, &cur, v, 1, __ATOMIC_RELAXED,
                                                   __ATOMIC_RELAXED)) { }
}

/* region slot for a construct address; -1 if the table is full */
static int region_slot(const void *codeptr)
{
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (g_regions[i].codeptr == codeptr) return i;
    pthread_mutex_lock(&g_region_lock);
    n = g_nregions;
    for (int i = 0; i < n; i++)
        if (g_regions[i].codeptr == codeptr) { pthread_mutex_unlock(&g_region_lock); return i; }
    int id = -1;
    if (n < OMP_MAX_REGIONS) {
        g_regions[n].codeptr = codeptr;
        id = n;
        __atomic_store_n(&g_nregions, n + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_region_lock);
    return id;
}

static omp_thread_t *this_thread(void)
{
    if (t.thr) return t.thr;
    int idx = __atomic_fetch_add(&g_nthreads, 1, __ATOMIC_ACQ_REL);
    if (idx >= OMP_MAX_THREADS) return NULL;
    t.thr = &g_threads[idx];
    return t.thr;
}

/* End time of an event in region instance `inst`: now, or the instance's
 * release time if the region already ended before `now`. */
static inline uint64_t clamp_end(int slot, uint64_t inst, uint64_t now)
{
    if (slot < 0) return now;
    if (__atomic_load_n(&g_rel_inst[slot], __ATOMIC_ACQUIRE) == inst) {
        uint64_t r = __atomic_load_n(&g_rel_ns[slot], __ATOMIC_RELAXED);
        if (r && r < now) return r;
    }
    return now;
}

/* ── callbacks ───────────────────────────────────────────────────────── */

static void on_thread_begin(int type, mp_ompt_data_t *thread_data)
{
    omp_thread_t *thr = this_thread();
    if (thr) thr->type = type;
    if (thread_data) thread_data->value = 0;
    cnt_thread_hook();          /* counters for this OpenMP thread */
}

static void on_thread_end(mp_ompt_data_t *thread_data) { (void)thread_data; }

static void on_parallel_begin(mp_ompt_data_t *enc_task, const mp_ompt_frame_t *enc_frame,
                              mp_ompt_data_t *parallel_data, unsigned int requested,
                              int flags, const void *codeptr)
{
    (void)enc_task; (void)enc_frame; (void)requested;
    if (flags & MP_OMPT_PARALLEL_LEAGUE) return;          /* teams: not a parallel region */
    int slot = region_slot(codeptr);
    if (parallel_data) parallel_data->value = (uint64_t)(slot + 1);
    if (t.pdepth < DEPTH) {
        t.par_t0[t.pdepth] = now_ns();
        t.par_slot[t.pdepth] = slot;
        t.par_inst[t.pdepth] = slot >= 0
            ? __atomic_add_fetch(&g_inst[slot], 1, __ATOMIC_ACQ_REL) : 0;
    }
    t.pdepth++;
    if (g_recording && slot >= 0) ADD(g_regions[slot].calls, 1);
}

static void on_parallel_end(mp_ompt_data_t *parallel_data, mp_ompt_data_t *enc_task,
                            int flags, const void *codeptr)
{
    (void)parallel_data; (void)enc_task; (void)codeptr;
    if (flags & MP_OMPT_PARALLEL_LEAGUE) return;
    if (t.pdepth <= 0) return;
    t.pdepth--;
    if (t.pdepth >= DEPTH) return;
    uint64_t now = now_ns();
    uint64_t dt = now - t.par_t0[t.pdepth];
    int slot = t.par_slot[t.pdepth];
    if (slot >= 0) {   /* publish the release: time first, then instance */
        __atomic_store_n(&g_rel_ns[slot], now, __ATOMIC_RELAXED);
        __atomic_store_n(&g_rel_inst[slot], t.par_inst[t.pdepth], __ATOMIC_RELEASE);
    }
    if (!g_recording) return;
    if (slot >= 0) ADD(g_regions[slot].wall_ns, dt);
    omp_thread_t *thr = this_thread();
    if (t.pdepth == 0 && thr && thr->type == MP_OMPT_THREAD_INITIAL)
        ADD(g_outer_parallel_ns, dt);
}

static void on_implicit_task(int endpoint, mp_ompt_data_t *parallel_data,
                             mp_ompt_data_t *task_data, unsigned int actual,
                             unsigned int index, int flags)
{
    (void)index;
    if (g_debug)
        fprintf(stderr, "[ompt] tid=%d implicit_task %s flags=0x%x index=%u t=%.3f ms tdepth=%d\n",
                (int)syscall(SYS_gettid), endpoint == MP_OMPT_SCOPE_BEGIN ? "begin" : "end", flags, index,
                now_ns() * 1e-6, t.tdepth);
    if (flags & MP_OMPT_TASK_INITIAL) return;            /* the initial task, not a team */
    if (endpoint == MP_OMPT_SCOPE_BEGIN) {
        int slot = parallel_data ? (int)parallel_data->value - 1 : -1;
        if (task_data) task_data->value = (uint64_t)(slot + 1);
        if (t.tdepth < DEPTH) {
            t.task_t0[t.tdepth] = now_ns();
            t.task_slot[t.tdepth] = slot;
            t.task_wait[t.tdepth] = 0;
            t.task_inst[t.tdepth] = slot >= 0
                ? __atomic_load_n(&g_inst[slot], __ATOMIC_ACQUIRE) : 0;
        }
        if (t.tdepth == 0) {
            cnt_omp_worker(1);                            /* worker counts: working */
            omp_thread_t *thr = this_thread();
            if (thr) {
                thr->slot = slot; thr->t0 = t.task_t0[0]; thr->inst = t.task_inst[0];
                thr->wait = 0; thr->in_wait = 0;
                __atomic_store_n(&thr->live, 1, __ATOMIC_RELEASE);
            }
        }
        t.tdepth++;
        if (g_recording && slot >= 0) {
            ADD(g_regions[slot].tasks, 1);
            atomic_max(&g_regions[slot].max_team, actual);
        }
    } else if (endpoint == MP_OMPT_SCOPE_END) {
        if (t.tdepth <= 0) return;
        t.tdepth--;
        if (t.tdepth >= DEPTH) return;
        int slot = t.task_slot[t.tdepth];
        uint64_t end = clamp_end(slot, t.task_inst[t.tdepth], now_ns());
        uint64_t dt = end > t.task_t0[t.tdepth] ? end - t.task_t0[t.tdepth] : 0;
        uint64_t w  = t.task_wait[t.tdepth];
        if (t.tdepth == 0 && t.thr && !__atomic_exchange_n(&t.thr->live, 0, __ATOMIC_ACQ_REL))
            return;                                       /* already flushed */
        if (!g_recording) return;
        if (slot >= 0) ADD(g_regions[slot].task_ns, dt);
        omp_thread_t *thr = this_thread();
        if (thr) {
            ADD(thr->work_ns, dt > w ? dt - w : 0);
            ADD(thr->wait_ns, w);
        }
    }
}

static int is_barrier(int kind)
{
    return kind == MP_OMPT_SYNC_BARRIER || kind == MP_OMPT_SYNC_BARRIER_IMPLICIT ||
           kind == MP_OMPT_SYNC_BARRIER_EXPLICIT ||
           kind == MP_OMPT_SYNC_BARRIER_IMPLEMENTATION ||
           kind == MP_OMPT_SYNC_BARRIER_IMPLICIT_WORKSHARE ||
           kind == MP_OMPT_SYNC_BARRIER_IMPLICIT_PARALLEL ||
           kind == MP_OMPT_SYNC_BARRIER_TEAMS;
}

static void on_sync_region_wait(int kind, int endpoint, mp_ompt_data_t *parallel_data,
                                mp_ompt_data_t *task_data, const void *codeptr)
{
    (void)parallel_data; (void)task_data; (void)codeptr;
    int barrier = is_barrier(kind);
    int taskw = (kind == MP_OMPT_SYNC_TASKWAIT || kind == MP_OMPT_SYNC_TASKGROUP);
    if (g_debug)
        fprintf(stderr, "[ompt] tid=%d sync_wait kind=%d %s t=%.3f ms depth=%d tdepth=%d\n",
                (int)syscall(SYS_gettid), kind, endpoint == MP_OMPT_SCOPE_BEGIN ? "begin" : "end",
                now_ns() * 1e-6, t.wait_depth, t.tdepth);
    if (!barrier && !taskw) return;
    /* libomp reports nested wait regions for one physical wait (e.g. the
     * implementation barrier inside a worksharing barrier); counting each
     * end would count the same wait twice. Only the outermost counts. */
    if (endpoint == MP_OMPT_SCOPE_BEGIN) {
        if (kind == MP_OMPT_SYNC_BARRIER_IMPLICIT_PARALLEL && t.tdepth == 1)
            cnt_omp_worker(0);                            /* region done: barrier, then pool */
        if (t.wait_depth++ == 0) {
            t.wait_t0 = now_ns(); t.wait_is_barrier = barrier;
            if (barrier && t.tdepth == 1 && t.thr) {
                t.thr->wait_t0 = t.wait_t0;
                __atomic_store_n(&t.thr->in_wait, 1, __ATOMIC_RELEASE);
            }
        }
        return;
    }
    if (endpoint != MP_OMPT_SCOPE_END || t.wait_depth <= 0) return;
    if (--t.wait_depth != 0) return;
    barrier = t.wait_is_barrier;
    uint64_t now = now_ns();
    uint64_t w = now - t.wait_t0;
    if (barrier) {
        /* attribute to the innermost implicit task this thread is running */
        if (t.tdepth > 0 && t.tdepth <= DEPTH) {
            int d = t.tdepth - 1;
            uint64_t end = clamp_end(t.task_slot[d], t.task_inst[d], now);
            w = end > t.wait_t0 ? end - t.wait_t0 : 0;
            t.task_wait[d] += w;
            if (d == 0 && t.thr) {
                if (!__atomic_load_n(&t.thr->live, __ATOMIC_ACQUIRE)) return;  /* flushed */
                t.thr->wait += w;
                __atomic_store_n(&t.thr->in_wait, 0, __ATOMIC_RELEASE);
            }
            if (g_recording && t.task_slot[d] >= 0) ADD(g_regions[t.task_slot[d]].wait_ns, w);
        }
    } else if (g_recording) {
        omp_thread_t *thr = this_thread();
        if (thr) ADD(thr->taskwait_ns, w);
    }
}

static void on_mutex_acquire(int kind, unsigned int hint, unsigned int impl,
                             mp_ompt_data_t wait_id, const void *codeptr)
{
    (void)kind; (void)hint; (void)impl; (void)wait_id; (void)codeptr;
    t.lock_t0 = now_ns();
}

static void on_mutex_acquired(int kind, mp_ompt_data_t wait_id, const void *codeptr)
{
    (void)kind; (void)wait_id; (void)codeptr;
    if (!g_recording || !t.lock_t0) return;
    omp_thread_t *thr = this_thread();
    if (thr) ADD(thr->lock_ns, now_ns() - t.lock_t0);
    t.lock_t0 = 0;
}

/* ── tool registration ───────────────────────────────────────────────── */

static void reg(int event, void *fn)
{
    if (g_set_cb) g_set_cb(event, (mp_ompt_callback_t)fn);
}

static int tool_initialize(mp_ompt_function_lookup_t lookup, int device,
                           mp_ompt_data_t *tool_data)
{
    (void)device; (void)tool_data;
    g_debug = getenv("MINIPROF_OMPT_DEBUG") != NULL;
    g_set_cb = (mp_ompt_set_callback_t)lookup("ompt_set_callback");
    if (!g_set_cb) return 0;    /* 0 = deactivate the tool */
    reg(MP_OMPT_CB_THREAD_BEGIN,     (void *)on_thread_begin);
    reg(MP_OMPT_CB_THREAD_END,       (void *)on_thread_end);
    reg(MP_OMPT_CB_PARALLEL_BEGIN,   (void *)on_parallel_begin);
    reg(MP_OMPT_CB_PARALLEL_END,     (void *)on_parallel_end);
    reg(MP_OMPT_CB_IMPLICIT_TASK,    (void *)on_implicit_task);
    reg(MP_OMPT_CB_SYNC_REGION_WAIT, (void *)on_sync_region_wait);
    reg(MP_OMPT_CB_MUTEX_ACQUIRE,    (void *)on_mutex_acquire);
    reg(MP_OMPT_CB_MUTEX_ACQUIRED,   (void *)on_mutex_acquired);
    g_started = 1;
    return 1;
}

static void tool_finalize(mp_ompt_data_t *tool_data) { (void)tool_data; }

/* Make the runtime find us. LLVM libomp's own ompt_start_tool is a weak,
 * versioned symbol that binds locally, and its fallback search is
 * dlsym(RTLD_NEXT) — it only sees tools loaded AFTER libomp, and
 * libminiprof is normally loaded before it. The portable discovery path
 * is OMP_TOOL_LIBRARIES, which libomp reads lazily at its first OpenMP
 * construct, so setting it from our constructor (before main) works:
 * the runtime dlopens libminiprof (already loaded: same handle) and calls
 * the ompt_start_tool below. Appends to any tool list the user set. */
__attribute__((constructor))
static void ompt_register_self(void)
{
    const char *mode = getenv("MINIPROF_MODE");
    const char *on = getenv("MINIPROF_OMPT");
    if (!mode || strcmp(mode, "snapshot") != 0) return;
    if (on && strcmp(on, "0") == 0) return;
    Dl_info di;
    if (!dladdr((void *)ompt_register_self, &di) || !di.dli_fname) return;
    const char *cur = getenv("OMP_TOOL_LIBRARIES");
    if (cur && strstr(cur, di.dli_fname)) return;
    char buf[4096];
    if (cur && *cur) snprintf(buf, sizeof(buf), "%s:%s", cur, di.dli_fname);
    else             snprintf(buf, sizeof(buf), "%s", di.dli_fname);
    setenv("OMP_TOOL_LIBRARIES", buf, 1);
}

/* Called by an OMPT-capable runtime at initialisation. Returning NULL
 * declines (any mode other than snapshot). */
mp_ompt_start_tool_result_t *ompt_start_tool(unsigned int omp_version,
                                             const char *runtime_version)
{
    (void)omp_version;
    const char *mode = getenv("MINIPROF_MODE");
    const char *on = getenv("MINIPROF_OMPT");
    if (!mode || strcmp(mode, "snapshot") != 0) return NULL;
    if (on && strcmp(on, "0") == 0) return NULL;
    snprintf(g_runtime, sizeof(g_runtime), "%s", runtime_version ? runtime_version : "");
    static mp_ompt_start_tool_result_t result = { tool_initialize, tool_finalize, { 0 } };
    return &result;
}

/* ── snapshot interface ──────────────────────────────────────────────── */

void ompt_snapshot_begin(void)
{
    /* keep region identities (codeptr), reset their statistics */
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const void *c = g_regions[i].codeptr;
        memset(&g_regions[i], 0, sizeof(g_regions[i]));
        g_regions[i].codeptr = c;
    }
    int nt = __atomic_load_n(&g_nthreads, __ATOMIC_ACQUIRE);
    if (nt > OMP_MAX_THREADS) nt = OMP_MAX_THREADS;
    for (int i = 0; i < nt; i++) {
        g_threads[i].work_ns = g_threads[i].wait_ns = 0;
        g_threads[i].lock_ns = g_threads[i].taskwait_ns = 0;
    }
    g_outer_parallel_ns = 0;
    __atomic_store_n(&g_recording, 1, __ATOMIC_RELEASE);
}

void ompt_snapshot_end(void)
{
    /* Flush workers parked at the join barrier of an already-finished
     * region: their wait ends at the region's release time. */
    uint64_t now = now_ns();
    int nt = __atomic_load_n(&g_nthreads, __ATOMIC_ACQUIRE);
    if (nt > OMP_MAX_THREADS) nt = OMP_MAX_THREADS;
    for (int i = 0; i < nt; i++) {
        omp_thread_t *th = &g_threads[i];
        if (th == t.thr || !__atomic_load_n(&th->live, __ATOMIC_ACQUIRE)) continue;
        if (!__atomic_load_n(&th->in_wait, __ATOMIC_ACQUIRE) || th->slot < 0) continue;
        uint64_t end = clamp_end(th->slot, th->inst, now);
        if (end >= now) continue;                 /* region still running */
        if (!__atomic_exchange_n(&th->live, 0, __ATOMIC_ACQ_REL)) continue;
        uint64_t w  = end > th->wait_t0 ? end - th->wait_t0 : 0;
        uint64_t dt = end > th->t0 ? end - th->t0 : 0;
        uint64_t tw = th->wait + w;
        ADD(g_regions[th->slot].wait_ns, w);
        ADD(g_regions[th->slot].task_ns, dt);
        ADD(th->work_ns, dt > tw ? dt - tw : 0);
        ADD(th->wait_ns, tw);
    }
    __atomic_store_n(&g_recording, 0, __ATOMIC_RELEASE);
}

void ompt_collect(omp_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!g_started) {
        out->available = 0;
        /* Say why: no OpenMP at all, or an OpenMP runtime without OMPT. */
        /* LLVM libomp also exports GOMP_* (GCC compatibility), so test for
         * its own entry point first */
        if (dlsym(RTLD_DEFAULT, "__kmpc_fork_call"))
            snprintf(out->reason, sizeof(out->reason),
                     "OpenMP runtime (LLVM libomp family) did not start the OMPT tool "
                     "(OMP_TOOL=disabled, MINIPROF_OMPT=0, or OMP_TOOL_LIBRARIES overridden)");
        else if (dlsym(RTLD_DEFAULT, "GOMP_parallel") || dlsym(RTLD_DEFAULT, "GOMP_parallel_start"))
            snprintf(out->reason, sizeof(out->reason),
                     "OpenMP runtime is GCC libgomp, which does not implement OMPT "
                     "(build with clang/AOCC/ICX to measure OpenMP)");
        else if (dlsym(RTLD_DEFAULT, "omp_get_num_threads"))
            snprintf(out->reason, sizeof(out->reason),
                     "OpenMP runtime did not start the OMPT tool "
                     "(OMP_TOOL=disabled, MINIPROF_OMPT=0, or no OMPT support)");
        else
            snprintf(out->reason, sizeof(out->reason), "application does not use OpenMP");
        return;
    }
    out->available = 1;
    snprintf(out->runtime, sizeof(out->runtime), "%s", g_runtime);
    out->outer_parallel_ns = __atomic_load_n(&g_outer_parallel_ns, __ATOMIC_RELAXED);

    int nt = __atomic_load_n(&g_nthreads, __ATOMIC_ACQUIRE);
    if (nt > OMP_MAX_THREADS) nt = OMP_MAX_THREADS;
    out->threads = nt;
    for (int i = 0; i < nt; i++) {
        const omp_thread_t *th = &g_threads[i];
        uint64_t w = __atomic_load_n(&th->work_ns, __ATOMIC_RELAXED);
        out->work_ns     += w;
        out->wait_ns     += __atomic_load_n(&th->wait_ns, __ATOMIC_RELAXED);
        out->lock_ns     += __atomic_load_n(&th->lock_ns, __ATOMIC_RELAXED);
        out->taskwait_ns += __atomic_load_n(&th->taskwait_ns, __ATOMIC_RELAXED);
        if (i == 0 || w < out->work_min_ns) out->work_min_ns = w;
        if (w > out->work_max_ns) out->work_max_ns = w;
    }
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    out->nregions = 0;
    for (int i = 0; i < n; i++) {
        omp_region_t r;
        r.codeptr  = g_regions[i].codeptr;
        r.calls    = __atomic_load_n(&g_regions[i].calls, __ATOMIC_RELAXED);
        r.wall_ns  = __atomic_load_n(&g_regions[i].wall_ns, __ATOMIC_RELAXED);
        r.task_ns  = __atomic_load_n(&g_regions[i].task_ns, __ATOMIC_RELAXED);
        r.wait_ns  = __atomic_load_n(&g_regions[i].wait_ns, __ATOMIC_RELAXED);
        r.tasks    = __atomic_load_n(&g_regions[i].tasks, __ATOMIC_RELAXED);
        r.max_team = __atomic_load_n(&g_regions[i].max_team, __ATOMIC_RELAXED);
        if (r.calls) out->regions[out->nregions++] = r;   /* only regions run in the window */
    }
}
