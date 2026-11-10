/*
 * nodemon_client.c — start/stop miniprof-nodemon from one rank per node.
 * See nodemon.h for the helper, the protocol and why it is separate.
 *
 * Helper location: $MINIPROF_NODEMON, else miniprof-nodemon next to
 * libminiprof.so. MINIPROF_MEMORY=0 skips it; MINIPROF_MEMORY_INTERVAL_MS
 * sets the sampling interval for peak bandwidth (default 100).
 *
 * Spawned with posix_spawn (vfork-based in glibc: safe in an MPI process
 * that has registered memory), with LD_PRELOAD removed from its
 * environment so it does not load libminiprof itself (a capability-
 * holding binary ignores LD_PRELOAD anyway).
 */

#define _GNU_SOURCE
#include "nodemon.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static pid_t g_pid = -1;
static int   g_in = -1, g_out = -1;          /* helper's stdin / stdout */
static char  g_reason[384] = "";
static int   g_interval = 100;
static int   g_verbose = 0;
static char  g_buf[16384];
static size_t g_len = 0;

/* Read one line from the helper, waiting at most timeout_ms. */
static int read_line(char *line, size_t n, int timeout_ms)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        char *nl = memchr(g_buf, '\n', g_len);
        if (nl) {
            size_t k = (size_t)(nl - g_buf);
            size_t c = k < n - 1 ? k : n - 1;
            memcpy(line, g_buf, c);
            line[c] = '\0';
            memmove(g_buf, nl + 1, g_len - k - 1);
            g_len -= k + 1;
            return 0;
        }
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0 || g_len >= sizeof(g_buf) - 1) return -1;
        struct pollfd p = { g_out, POLLIN, 0 };
        if (poll(&p, 1, left) <= 0) return -1;
        ssize_t r = read(g_out, g_buf + g_len, sizeof(g_buf) - 1 - g_len);
        if (r <= 0) return -1;
        g_len += (size_t)r;
    }
}

static void reap(void)
{
    if (g_in >= 0) close(g_in);
    if (g_out >= 0) close(g_out);
    g_in = g_out = -1;
    if (g_pid > 0) {
        int st;   /* it exits right after its last line: poll in 1 ms steps, <= 0.5 s */
        for (int i = 0; i < 500 && waitpid(g_pid, &st, WNOHANG) == 0; i++) usleep(1000);
        if (waitpid(g_pid, &st, WNOHANG) == 0) { kill(g_pid, SIGKILL); waitpid(g_pid, &st, 0); }
    }
    g_pid = -1;
}

static int helper_path(char *path, size_t n)
{
    const char *env = getenv("MINIPROF_NODEMON");
    if (env && *env) { snprintf(path, n, "%s", env); return 0; }
    Dl_info di;
    if (!dladdr((void *)helper_path, &di) || !di.dli_fname) return -1;
    const char *slash = strrchr(di.dli_fname, '/');
    if (!slash) snprintf(path, n, "./miniprof-nodemon");
    else snprintf(path, n, "%.*s/miniprof-nodemon", (int)(slash - di.dli_fname), di.dli_fname);
    return 0;
}

void nodemon_start(const char *uarch, int verbose)
{
    g_verbose = verbose;
    const char *mem = getenv("MINIPROF_MEMORY");
    if (mem && strcmp(mem, "0") == 0) {
        snprintf(g_reason, sizeof(g_reason), "disabled (MINIPROF_MEMORY=0)");
        return;
    }
    const char *ua = getenv("MINIPROF_NODEMON_TEST") ? "swtest" : uarch;
    if (!ua || (strcmp(ua, "zen4") && strcmp(ua, "zen5") && strcmp(ua, "swtest"))) {
        snprintf(g_reason, sizeof(g_reason),
                 "no node-level memory counters for uarch '%s' (AMD Zen 4 / Zen 5 only)",
                 ua && *ua ? ua : "unknown");
        return;
    }
    const char *iv = getenv("MINIPROF_MEMORY_INTERVAL_MS");
    if (iv && atoi(iv) >= 10 && atoi(iv) <= 10000) g_interval = atoi(iv);

    char path[1024];
    if (helper_path(path, sizeof(path)) != 0 || access(path, X_OK) != 0) {
        snprintf(g_reason, sizeof(g_reason),
                 "helper not found at %.200s: build it (make nodemon) and grant it "
                 "CAP_PERFMON (sudo setcap cap_perfmon+ep <path>), or set MINIPROF_NODEMON",
                 path);
        return;
    }

    /* Do not start a helper that cannot work: without CAP_PERFMON on the
     * file (security.capability xattr), root, or perf_event_paranoid <= 0,
     * it would only fail — and starting it costs rank 0 time. */
    if (strcmp(ua, "swtest") != 0 && geteuid() != 0 &&
        getxattr(path, "security.capability", NULL, 0) <= 0) {
        int par = 2;
        FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
        if (f) { if (fscanf(f, "%d", &par) != 1) par = 2; fclose(f); }
        if (par > 0) {
            snprintf(g_reason, sizeof(g_reason),
                     "helper %.200s has no CAP_PERFMON (perf_event_paranoid=%d): "
                     "sudo setcap cap_perfmon+ep <path> (make nodemon-setcap)", path, par);
            return;
        }
    }

    int pin[2], pout[2];
    if (pipe2(pin, O_CLOEXEC) != 0) { snprintf(g_reason, sizeof(g_reason), "pipe: %s", strerror(errno)); return; }
    if (pipe2(pout, O_CLOEXEC) != 0) {
        close(pin[0]); close(pin[1]);
        snprintf(g_reason, sizeof(g_reason), "pipe: %s", strerror(errno));
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
    posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pout[1], 2);

    /* environment without LD_PRELOAD */
    int ne = 0;
    while (environ[ne]) ne++;
    char **envp = calloc((size_t)ne + 1, sizeof(char *));
    int k = 0;
    for (int i = 0; envp && i < ne; i++)
        if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0) envp[k++] = environ[i];

    char ivs[16];
    snprintf(ivs, sizeof(ivs), "%d", g_interval);
    char *argv[] = { path, "--uarch", (char *)ua, "--interval-ms", ivs, NULL };
    int rc = posix_spawn(&g_pid, path, &fa, NULL, argv, envp ? envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    free(envp);
    close(pin[0]); close(pout[1]);
    g_in = pin[1]; g_out = pout[0];
    if (rc != 0) {
        snprintf(g_reason, sizeof(g_reason), "cannot start %.160s: %s", path, strerror(rc));
        g_pid = -1;
        reap();
        return;
    }
    char line[512];
    if (read_line(line, sizeof(line), 5000) != 0) {
        snprintf(g_reason, sizeof(g_reason), "helper %.160s did not answer", path);
        reap();
        return;
    }
    if (strncmp(line, "ok ", 3) != 0) {
        snprintf(g_reason, sizeof(g_reason), "%.380s",
                 strncmp(line, "error ", 6) == 0 ? line + 6 : line);
        reap();
        return;
    }
    if (g_verbose) fprintf(stderr, "[miniprof] nodemon: %s\n", line);
}

void nodemon_stop(nm_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->interval_ms = g_interval;
    if (g_pid <= 0) {
        snprintf(out->reason, sizeof(out->reason), "%s", g_reason[0] ? g_reason : "not started");
        return;
    }
    /* the helper may have died: never let a write kill the application */
    struct sigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);
    ssize_t w = write(g_in, "stop\n", 5);
    (void)w;
    sigaction(SIGPIPE, &old, NULL);

    char line[512];
    int got_result = 0;
    while (read_line(line, sizeof(line), 5000) == 0) {
        if (strcmp(line, "end") == 0) break;
        unsigned long long ens; int smp, mux;
        int s, c;
        unsigned long long rd, wr, rin, en;
        double prd, pwr, pn;
        if (sscanf(line, "result elapsed_ns=%llu samples=%d multiplexed=%d", &ens, &smp, &mux) == 3) {
            out->elapsed_s = ens * 1e-9; out->samples = smp; out->multiplexed = mux;
            got_result = 1;
        } else if (sscanf(line, "socket %d rd=%llu wr=%llu remote_in=%llu energy_uj=%llu peak_rd=%lf peak_wr=%lf",
                          &s, &rd, &wr, &rin, &en, &prd, &pwr) == 7 && s >= 0 && s < NM_MAX_SOCKETS) {
            nm_socket_t *so = &out->s[s];
            so->rd_bytes = rd; so->wr_bytes = wr; so->remote_in_bytes = rin; so->energy_uj = en;
            so->peak_rd_bps = prd; so->peak_wr_bps = pwr;
            if (s + 1 > out->nsockets) out->nsockets = s + 1;
        } else if (sscanf(line, "chan %d %d rd=%llu wr=%llu", &s, &c, &rd, &wr) == 4 &&
                   s >= 0 && s < NM_MAX_SOCKETS && c >= 0 && c < NM_MAX_CHANS) {
            out->s[s].chan_rd[c] = rd; out->s[s].chan_wr[c] = wr;
            if (c + 1 > out->s[s].nchans) out->s[s].nchans = c + 1;
        } else if (sscanf(line, "peak node=%lf", &pn) == 1) {
            out->peak_node_bps = pn;
        }
    }
    reap();
    if (got_result && out->nsockets > 0) out->available = 1;
    else snprintf(out->reason, sizeof(out->reason), "helper stopped without a result");
}
