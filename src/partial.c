/*
 * partial.c — per-rank record when a run ends without MPI_Finalize.
 * See partial.h for when and why; tools/miniprof_partial.py merges the
 * records into a snapshot marked partial.
 *
 * Signal safety: partial_dump may run inside a SIGTERM handler that
 * interrupted anything, including malloc or a lock holder. It therefore
 * uses no stdio, no malloc and no locks: strings were copied at init,
 * numbers are written as integers by hand into a static buffer, and the
 * file is written with open/write/close/rename (async-signal-safe). Counts
 * come from relaxed atomic loads, so a value may be one update old.
 *
 * After writing, the signal goes on as if miniprof were not there: to the
 * application's own handler if it installed one before MPI_Init (common:
 * checkpoint on SIGTERM), else the default action is restored and the
 * signal re-raised, so the exit status and the scheduler see the same end.
 */
#define _GNU_SOURCE
#include "partial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/miniprof.h"
#include "build_info.inc"
#include "counters.h"
#include "io_trace.h"
#include "mpi_trace.h"

/* ── state fixed at init ─────────────────────────────────────────────── */
static int  g_on = 0;                 /* prepared and enabled              */
static int  g_armed = 0;              /* between MPI_Init and MPI_Finalize */
static int  g_dumped = 0;             /* atomic: one record per process    */
static int  g_rank = 0, g_nranks = 0, g_cpus = 0, g_cores = 0;
static char g_dir[1024], g_path[1100], g_tmp[1100];
static char g_level[16], g_start_utc[32], g_cmd[512];
static char g_host[256], g_uarch[32], g_uarch_name[64], g_brand[128];
static struct timespec g_t0;          /* the application's clock start      */
static struct rusage   g_ru0;
static struct sigaction g_old_term, g_old_int;

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void partial_prepare(const partial_info_t *in)
{
    const char *e = getenv("MINIPROF_PARTIAL");
    if (e && *e == '0') return;
    if (!in->output || !*in->output) return;
    g_rank = in->rank;
    g_nranks = in->nranks;
    g_cpus = in->cpus_allowed;
    g_cores = in->cores_allowed;
    snprintf(g_dir, sizeof g_dir, "%s.partial", in->output);
    snprintf(g_path, sizeof g_path, "%s/rank.%d.json", g_dir, in->rank);
    snprintf(g_tmp, sizeof g_tmp, "%s/.rank.%d.tmp", g_dir, in->rank);
    copy(g_level, sizeof g_level, in->level);
    copy(g_start_utc, sizeof g_start_utc, in->start_utc);
    copy(g_cmd, sizeof g_cmd, in->command);
    copy(g_host, sizeof g_host, in->hostname);
    copy(g_uarch, sizeof g_uarch, in->uarch);
    copy(g_uarch_name, sizeof g_uarch_name, in->uarch_name);
    copy(g_brand, sizeof g_brand, in->brand);
    g_on = 1;
    /* records left by an earlier run with the same output path would be
     * merged with this run's: clear them now (records are only written at
     * the end, so nothing of this run is there yet) */
    if (g_rank == 0) {
        DIR *d = opendir(g_dir);
        if (d) {
            struct dirent *e;
            char f[1400];
            while ((e = readdir(d)))
                if (strncmp(e->d_name, "rank.", 5) == 0 || strncmp(e->d_name, ".rank.", 6) == 0) {
                    snprintf(f, sizeof f, "%s/%s", g_dir, e->d_name);
                    unlink(f);
                }
            closedir(d);
            rmdir(g_dir);
        }
    }
}

/* ── async-signal-safe output ────────────────────────────────────────── */
static char   g_buf[64 * 1024];
static size_t g_len;

static void put(const char *s)
{
    while (*s && g_len < sizeof g_buf - 1) g_buf[g_len++] = *s++;
}
static void put_u64(uint64_t v)
{
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && g_len < sizeof g_buf - 1) g_buf[g_len++] = t[--n];
}
static void put_str(const char *s)              /* JSON string, escaped */
{
    static const char hex[] = "0123456789abcdef";
    put("\"");
    for (; *s && g_len < sizeof g_buf - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { g_buf[g_len++] = '\\'; g_buf[g_len++] = (char)c; }
        else if (c < 0x20) {
            put("\\u00");
            g_buf[g_len++] = hex[c >> 4];
            g_buf[g_len++] = hex[c & 15];
        } else g_buf[g_len++] = (char)c;
    }
    put("\"");
}
static void key(const char *k) { put_str(k); put(":"); }
static void kv_u(const char *k, uint64_t v) { key(k); put_u64(v); }
static void kv_s(const char *k, const char *v) { key(k); put_str(v); }

static uint64_t ts_ns(const struct timespec *t) { return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec; }
static uint64_t tv_us(const struct timeval *t)  { return (uint64_t)t->tv_sec * 1000000ull + (uint64_t)t->tv_usec; }

static void put_io(const char *k, const io_counters_t *c)
{
    key(k); put("{");
    kv_u("open_calls", c->open_calls);   put(",");
    kv_u("close_calls", c->close_calls); put(",");
    kv_u("read_calls", c->read_calls);   put(",");
    kv_u("write_calls", c->write_calls); put(",");
    kv_u("sync_calls", c->sync_calls);   put(",");
    kv_u("read_bytes", c->read_bytes);   put(",");
    kv_u("write_bytes", c->write_bytes); put(",");
    kv_u("read_ns", c->read_ns);         put(",");
    kv_u("write_ns", c->write_ns);       put(",");
    kv_u("meta_ns", c->meta_ns);
    put("}");
}

static mpi_snap_t g_snap;                 /* static: too big for a signal stack */

void partial_dump(const char *reason, int signo)
{
    if (!g_on || !__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&g_dumped, 1, __ATOMIC_ACQ_REL)) return;
    int saved = errno;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed_ns = ts_ns(&now) - ts_ns(&g_t0);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    mpi_trace_snapshot_get(&g_snap);
    io_snap_t io;
    io_trace_get(&io);

    g_len = 0;
    put("{");
    kv_s("schema", "miniprof-partial");   put(",");
    kv_u("schema_version", 1);            put(",");
    key("tool"); put("{"); kv_s("version", MINIPROF_VERSION); put(",");
    kv_s("commit", MINIPROF_BUILD_COMMIT); put("},");
    kv_s("reason", reason);               put(",");
    kv_u("signal", (uint64_t)(signo > 0 ? signo : 0)); put(",");
    kv_u("rank", (uint64_t)g_rank);       put(",");
    kv_u("nranks", (uint64_t)g_nranks);   put(",");
    kv_s("hostname", g_host);             put(",");
    kv_s("level", g_level);               put(",");
    kv_s("start_utc", g_start_utc);       put(",");
    kv_s("command", g_cmd);               put(",");
    key("cpu"); put("{"); kv_s("uarch", g_uarch); put(",");
    kv_s("uarch_name", g_uarch_name); put(","); kv_s("brand", g_brand); put(",");
    kv_u("cpus_allowed", (uint64_t)g_cpus); put(",");
    kv_u("cores_allowed", (uint64_t)g_cores); put("},");
    kv_u("elapsed_ns", elapsed_ns);       put(",");
    kv_u("cpu_user_us", tv_us(&ru.ru_utime) - tv_us(&g_ru0.ru_utime)); put(",");
    kv_u("cpu_sys_us", tv_us(&ru.ru_stime) - tv_us(&g_ru0.ru_stime));  put(",");
    kv_u("max_rss_kb", (uint64_t)ru.ru_maxrss); put(",");

    /* MPI: [name, class, calls, bytes, ns] for every function called */
    key("mpi"); put("[");
    int first = 1;
    for (int f = 0; f < MPIF_COUNT; f++) {
        if (!g_snap.calls[f]) continue;
        if (!first) put(",");
        first = 0;
        put("["); put_str(mpi_trace_func_name(f)); put(",");
        put_str(mpi_trace_class_name(mpi_trace_func_class(f))); put(",");
        put_u64(g_snap.calls[f]); put(","); put_u64(g_snap.bytes[f]); put(",");
        put_u64(g_snap.ns[f]); put("]");
    }
    put("],");
    key("hist"); put("{");
    for (int c = 0; c < MPIC_COUNT; c++) {
        if (c) put(",");
        key(mpi_trace_class_name(c)); put("[");
        for (int b = 0; b < MPI_HIST_BINS; b++) {
            if (b) put(",");
            put("["); put_u64(g_snap.hist_calls[c][b]); put(",");
            put_u64(g_snap.hist_ns[c][b]); put("]");
        }
        put("]");
    }
    put("},");
    key("io"); put("{"); put_io("posix", &io.posix); put(","); put_io("mpiio", &io.mpiio); put("},");

    /* hardware counters, by event key: [compute, mpi] */
    key("counters"); put("{");
    const cnt_group_t *g = NULL;
    uint64_t cc[CNT_MAX_EVENTS], cm[CNT_MAX_EVENTS];
    int ne = cnt_peek(&g, cc, cm);
    if (ne > 0 && g) {
        kv_s("group", g->group); put(",");
        key("events"); put("{");
        for (int i = 0; i < ne; i++) {
            if (i) put(",");
            key(cnt_kind_name(g->ev[i].kind));
            put("["); put_u64(cc[i]); put(","); put_u64(cm[i]); put("]");
        }
        put("}");
    }
    put("}}\n");

    /* write the record, then rename it into place (readers never see half);
     * on another node the snapshot's folder may not exist yet: mkdir -p */
    for (char *p = g_dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(g_dir, 0755);
        *p = '/';
    }
    if (mkdir(g_dir, 0755) == 0 || errno == EEXIST) {
        int fd = open(g_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            size_t off = 0;
            while (off < g_len) {
                ssize_t w = write(fd, g_buf + off, g_len - off);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) break;
                off += (size_t)w;
            }
            close(fd);
            if (off == g_len) rename(g_tmp, g_path);
            else unlink(g_tmp);
        }
    }
    errno = saved;
}

/* ── triggers ────────────────────────────────────────────────────────── */
static void on_signal(int sig, siginfo_t *si, void *uc)
{
    partial_dump(sig == SIGTERM ? "SIGTERM" : "SIGINT", sig);
    const struct sigaction *old = sig == SIGTERM ? &g_old_term : &g_old_int;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) { old->sa_sigaction(sig, si, uc); return; }
    } else if (old->sa_handler == SIG_IGN) {
        return;
    } else if (old->sa_handler != SIG_DFL) {
        old->sa_handler(sig);
        return;
    }
    /* default action: put it back and re-raise; delivered once we return */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void on_exit_hook(void)
{
    partial_dump("exit without MPI_Finalize", 0);
}

void partial_arm(const struct timespec *app_start)
{
    if (!g_on) return;
    g_t0 = *app_start;
    getrusage(RUSAGE_SELF, &g_ru0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, &g_old_term);
    sigaction(SIGINT, &sa, &g_old_int);
    static int hooked = 0;
    if (!hooked) hooked = atexit(on_exit_hook) == 0;
    __atomic_store_n(&g_armed, 1, __ATOMIC_RELEASE);
}

void partial_disarm(void)
{
    if (!g_on || !__atomic_exchange_n(&g_armed, 0, __ATOMIC_ACQ_REL)) return;
    /* put back what was there, unless someone replaced ours meanwhile */
    struct sigaction cur;
    if (sigaction(SIGTERM, NULL, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_signal)
        sigaction(SIGTERM, &g_old_term, NULL);
    if (sigaction(SIGINT, NULL, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_signal)
        sigaction(SIGINT, &g_old_int, NULL);
}
