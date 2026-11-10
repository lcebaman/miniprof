/*
 * nodemon.c — miniprof-nodemon: socket-level memory and energy counters
 *
 * The one part of miniprof that needs a privilege: CAP_PERFMON, to open
 * system-wide events on the uncore PMUs (see nodemon.h for what and why).
 * Grant it to this binary only:
 *     sudo setcap cap_perfmon+ep ./miniprof-nodemon
 *
 * Kept deliberately small and closed:
 *   - takes no paths, event lists or other free-form input: only --uarch
 *     (zen4|zen5) and --interval-ms (10..10000);
 *   - opens a fixed set of events on the amd_umc_*, amd_df and power PMUs,
 *     found through sysfs, then DROPS all capabilities before sampling;
 *   - counts only node-wide hardware events: it cannot attach to, read
 *     or sample any process;
 *   - talks over stdin/stdout to the rank that started it; exits on
 *     "stop", on EOF, or when its parent dies;
 *   - reports only run totals and peaks over >= 10 ms intervals. Still,
 *     with the capability it exposes node-wide energy and memory traffic
 *     that the kernel keeps from unprivileged users (RAPL is root-only
 *     because of the PLATYPUS side channel): keep it owner- or
 *     group-executable only (make nodemon-setcap does chmod 0700).
 *
 * Encodings were checked against perf's own (perf stat -vv) on EPYC 9754
 * (Zen 4) and 9555 (Zen 5) with tools/pmu_probe_node.sh --sudo.
 */

#define _GNU_SOURCE
#include "nodemon.h"

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <linux/capability.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SYSPMU "/sys/bus/event_source/devices"
#define MAX_EV 512

typedef enum { EV_UMC_RD, EV_UMC_WR, EV_DF_REMOTE_IN, EV_ENERGY } ev_kind_t;

typedef struct {
    int       fd;
    ev_kind_t kind;
    int       socket, chan;
    double    scale;               /* bytes (or uJ) per count */
    uint64_t  v0, last;            /* first and previous (scaled) values */
} ev_t;

static ev_t g_ev[MAX_EV];
static int  g_nev = 0;
static int  g_nsockets = 0;
static int  g_nchans[NM_MAX_SOCKETS];
static int  g_mux = 0;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("error ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    exit(1);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int pmu_type(const char *pmu)
{
    char p[256], b[64];
    snprintf(p, sizeof(p), SYSPMU "/%s/type", pmu);
    return read_file(p, b, sizeof(b)) == 0 ? atoi(b) : -1;
}

/* the PMU's cpumask: CPUs (one per socket for uncore PMUs) */
static int pmu_cpus(const char *pmu, int *cpus, int max)
{
    char p[256], b[1024];
    snprintf(p, sizeof(p), SYSPMU "/%s/cpumask", pmu);
    if (read_file(p, b, sizeof(b)) != 0) return 0;
    int n = 0;
    for (char *t = strtok(b, ","); t && n < max; t = strtok(NULL, ",")) {
        int a = atoi(t), z = a;
        char *d = strchr(t, '-');
        if (d) z = atoi(d + 1);
        for (int c = a; c <= z && n < max; c++) cpus[n++] = c;
    }
    return n;
}

static int cpu_socket(int cpu)
{
    char p[256], b[64];
    snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
    return read_file(p, b, sizeof(b)) == 0 ? atoi(b) : -1;
}

/* socket id -> dense index 0..n-1 */
static int g_sock_ids[NM_MAX_SOCKETS];
static int sock_index(int id)
{
    for (int i = 0; i < g_nsockets; i++) if (g_sock_ids[i] == id) return i;
    if (g_nsockets >= NM_MAX_SOCKETS) die("more than %d sockets", NM_MAX_SOCKETS);
    g_sock_ids[g_nsockets] = id;
    return g_nsockets++;
}

static int g_self_only = 0;   /* swtest: count this process (no privilege needed) */

static void open_ev(int type, uint64_t config, int cpu, ev_kind_t kind, int sock, int chan,
                    double scale, const char *what)
{
    if (g_nev >= MAX_EV) die("too many events");
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.type = (uint32_t)type;
    a.config = config;
    a.disabled = 1;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    /* uncore events cannot filter by privilege level: exclude_* stay 0 */
    int fd = g_self_only ? (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0)
                         : (int)syscall(__NR_perf_event_open, &a, -1, cpu, -1, 0);
    if (fd < 0) {
        int e = errno;
        if (g_self_only)   /* swtest: plain per-process software events */
            die("cannot open %s: %s (software perf events blocked: perf_event_paranoid?)",
                what, strerror(e));
        die("cannot open %s on cpu %d: %s%s", what, cpu, strerror(e),
            (e == EACCES || e == EPERM)
                ? " — system-wide counters need CAP_PERFMON: sudo setcap cap_perfmon+ep "
                  "<path to miniprof-nodemon> (or perf_event_paranoid <= 0)" : "");
    }
    ev_t *v = &g_ev[g_nev++];
    memset(v, 0, sizeof(*v));
    v->fd = fd; v->kind = kind; v->socket = sock; v->chan = chan; v->scale = scale;
}

/* DF config: event[7:0] -> config[7:0], event[13:8] -> config[37:32],
 * umask[7:0] -> config[15:8], umask[11:8] -> config[27:24] (amd_df format) */
static uint64_t df_config(unsigned ev, unsigned um)
{
    return (uint64_t)(ev & 0xff) | ((uint64_t)((ev >> 8) & 0x3f) << 32) |
           ((uint64_t)(um & 0xff) << 8) | ((uint64_t)((um >> 8) & 0xf) << 24);
}

static int cmp_umc(const void *a, const void *b)
{
    return atoi((const char *)a + 8) - atoi((const char *)b + 8);   /* "amd_umc_N" */
}

/* Test-only (--uarch swtest): software events of the helper itself, so
 * the pipe protocol and the snapshot's memory block can be exercised on
 * machines without these PMUs (CI). The numbers mean nothing. */
static void setup_swtest(void)
{
    g_self_only = 1;
    int s = sock_index(0);
    for (int c = 0; c < 2; c++) {
        g_nchans[s]++;
        open_ev(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, -1, EV_UMC_RD, s, c, 1.0, "task-clock");
        open_ev(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, -1, EV_UMC_WR, s, c, 64.0, "cs");
    }
    open_ev(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS, -1, EV_DF_REMOTE_IN, s, 0, 32.0, "mig");
}

static void setup(const char *uarch)
{
    if (strcmp(uarch, "swtest") == 0) { setup_swtest(); return; }
    int zen5 = strcmp(uarch, "zen5") == 0;
    double umc_bytes = zen5 ? 64.0 : 32.0;

    /* memory controllers: one PMU per channel, cpumask = a CPU of its socket */
    static char names[64][32];
    int nu = 0;
    DIR *d = opendir(SYSPMU);
    if (!d) die("no %s", SYSPMU);
    struct dirent *de;
    while ((de = readdir(d)) && nu < 64)
        if (strncmp(de->d_name, "amd_umc_", 8) == 0)
            snprintf(names[nu++], sizeof(names[0]), "%.31s", de->d_name);
    closedir(d);
    if (nu == 0) die("no amd_umc PMUs (kernel without AMD memory-controller PMU support)");
    qsort(names, (size_t)nu, sizeof(names[0]), cmp_umc);
    for (int i = 0; i < nu; i++) {
        int type = pmu_type(names[i]), cpu;
        if (type < 0 || pmu_cpus(names[i], &cpu, 1) != 1) continue;
        int s = sock_index(cpu_socket(cpu));
        int c = g_nchans[s]++;
        if (c >= NM_MAX_CHANS) die("more than %d memory channels per socket", NM_MAX_CHANS);
        open_ev(type, 0x10a, cpu, EV_UMC_RD, s, c, umc_bytes, names[i]);   /* umc_cas_cmd.rd */
        open_ev(type, 0x20a, cpu, EV_UMC_WR, s, c, umc_bytes, names[i]);   /* umc_cas_cmd.wr */
    }

    /* data fabric: data into this socket's cores from the other socket,
     * 32 B per beat. Zen 5: remote_socket_inbound_data_beats_cfi_0..15;
     * Zen 4: remote_socket_inf0_inbound_data_beats_ccm0..7 (the inf1 half
     * counted 0 in every test and its encoding is unverified). */
    int dtype = pmu_type("amd_df"), dcpus[NM_MAX_SOCKETS];
    int nd = dtype >= 0 ? pmu_cpus("amd_df", dcpus, NM_MAX_SOCKETS) : 0;
    int nport = zen5 ? 16 : 8;
    for (int k = 0; k < nd; k++) {
        int s = sock_index(cpu_socket(dcpus[k]));
        for (int n = 0; n < nport; n++) {
            unsigned ev = 0x41e + 0x40 * (n % 4) + 0x100 * ((n / 4) % 2) + (n / 8);
            open_ev(dtype, df_config(ev, 0xbfe), dcpus[k], EV_DF_REMOTE_IN, s, n, 32.0, "amd_df");
        }
    }

    /* package energy, in microjoules */
    int ptype = pmu_type("power"), pcpus[NM_MAX_SOCKETS];
    int np = ptype >= 0 ? pmu_cpus("power", pcpus, NM_MAX_SOCKETS) : 0;
    char b[128];
    unsigned cfg = 0;
    double scale = 0;
    if (np > 0 && read_file(SYSPMU "/power/events/energy-pkg", b, sizeof(b)) == 0) {
        char *e = strstr(b, "event=");
        cfg = e ? (unsigned)strtoul(e + 6, NULL, 0) : 0;
        if (read_file(SYSPMU "/power/events/energy-pkg.scale", b, sizeof(b)) == 0) scale = atof(b);
    }
    if (cfg && scale > 0)
        for (int k = 0; k < np; k++)
            open_ev(ptype, cfg, pcpus[k], EV_ENERGY, sock_index(cpu_socket(pcpus[k])), 0,
                    scale * 1e6, "power/energy-pkg");
}

/* Drop every capability: from here on the helper can only read the fds
 * it already holds. */
static void drop_caps(void)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct dd[2];
    memset(dd, 0, sizeof(dd));
    if (syscall(SYS_capset, &h, dd) != 0) die("capset: %s", strerror(errno));
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
}

static uint64_t read_ev(ev_t *v)
{
    uint64_t r[3] = {0, 0, 0};
    if (read(v->fd, r, sizeof(r)) != (ssize_t)sizeof(r)) return v->last;
    double val = (double)r[0];
    if (r[2] > 0 && r[2] < r[1]) { val *= (double)r[1] / (double)r[2]; g_mux = 1; }
    return (uint64_t)(val * v->scale);
}

int main(int argc, char **argv)
{
    const char *uarch = NULL;
    int interval = 100;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uarch") == 0 && i + 1 < argc) uarch = argv[++i];
        else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) interval = atoi(argv[++i]);
        else die("usage: miniprof-nodemon --uarch zen4|zen5 [--interval-ms 10..10000]");
    }
    if (!uarch || (strcmp(uarch, "zen4") != 0 && strcmp(uarch, "zen5") != 0 &&
                   strcmp(uarch, "swtest") != 0))
        die("unsupported uarch '%s' (zen4, zen5)", uarch ? uarch : "");
    if (interval < 10 || interval > 10000) die("interval-ms out of range");
    setvbuf(stdout, NULL, _IOLBF, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);   /* never outlive the rank */
    signal(SIGPIPE, SIG_IGN);

    setup(uarch);
    drop_caps();

    for (int i = 0; i < g_nev; i++) ioctl(g_ev[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    uint64_t t0 = now_ns(), tprev = t0;
    for (int i = 0; i < g_nev; i++) g_ev[i].v0 = g_ev[i].last = read_ev(&g_ev[i]);
    int maxc = 0;
    for (int s = 0; s < g_nsockets; s++) if (g_nchans[s] > maxc) maxc = g_nchans[s];
    printf("ok sockets=%d chans=%d interval_ms=%d events=%d\n", g_nsockets, maxc, interval, g_nev);

    double peak_rd[NM_MAX_SOCKETS] = {0}, peak_wr[NM_MAX_SOCKETS] = {0}, peak_node = 0;
    int samples = 0;
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        int r = poll(&p, 1, interval);
        int stop = 0;
        if (r > 0) {
            char buf[64];
            ssize_t k = read(0, buf, sizeof(buf));
            stop = (k <= 0) || memmem(buf, (size_t)k, "stop", 4) != NULL;
        } else if (r < 0 && errno != EINTR) {
            stop = 1;
        }
        uint64_t t = now_ns();
        double dt = (t - tprev) * 1e-9;
        double rd[NM_MAX_SOCKETS] = {0}, wr[NM_MAX_SOCKETS] = {0};
        for (int i = 0; i < g_nev; i++) {
            ev_t *v = &g_ev[i];
            uint64_t x = read_ev(v);
            double dlt = (double)(x - v->last);
            v->last = x;
            if (v->kind == EV_UMC_RD) rd[v->socket] += dlt;
            if (v->kind == EV_UMC_WR) wr[v->socket] += dlt;
        }
        tprev = t;
        if (dt > interval * 0.5e-3) {          /* ignore a sliver before "stop" */
            double node = 0;
            for (int s = 0; s < g_nsockets; s++) {
                if (rd[s] / dt > peak_rd[s]) peak_rd[s] = rd[s] / dt;
                if (wr[s] / dt > peak_wr[s]) peak_wr[s] = wr[s] / dt;
                node += (rd[s] + wr[s]) / dt;
            }
            if (node > peak_node) peak_node = node;
            samples++;
        }
        if (stop) break;
    }

    uint64_t t1 = now_ns();
    printf("result elapsed_ns=%llu samples=%d multiplexed=%d\n",
           (unsigned long long)(t1 - t0), samples, g_mux);
    for (int s = 0; s < g_nsockets; s++) {
        uint64_t rd = 0, wr = 0, rin = 0, en = 0;
        for (int i = 0; i < g_nev; i++) {
            ev_t *v = &g_ev[i];
            if (v->socket != s) continue;
            uint64_t d = v->last - v->v0;
            if (v->kind == EV_UMC_RD) rd += d;
            if (v->kind == EV_UMC_WR) wr += d;
            if (v->kind == EV_DF_REMOTE_IN) rin += d;
            if (v->kind == EV_ENERGY) en += d;
        }
        printf("socket %d rd=%llu wr=%llu remote_in=%llu energy_uj=%llu peak_rd=%.6g peak_wr=%.6g\n",
               s, (unsigned long long)rd, (unsigned long long)wr, (unsigned long long)rin,
               (unsigned long long)en, peak_rd[s], peak_wr[s]);
        for (int c = 0; c < g_nchans[s]; c++) {
            uint64_t crd = 0, cwr = 0;
            for (int i = 0; i < g_nev; i++) {
                ev_t *v = &g_ev[i];
                if (v->socket != s || v->chan != c) continue;
                if (v->kind == EV_UMC_RD) crd = v->last - v->v0;
                if (v->kind == EV_UMC_WR) cwr = v->last - v->v0;
            }
            printf("chan %d %d rd=%llu wr=%llu\n", s, c, (unsigned long long)crd,
                   (unsigned long long)cwr);
        }
    }
    printf("peak node=%.6g\nend\n", peak_node);
    return 0;
}
