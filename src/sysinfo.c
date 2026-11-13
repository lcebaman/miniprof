/*
 * sysinfo.c — node and rank metadata for snapshot mode. See sysinfo.h.
 */

#define _GNU_SOURCE
#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sched.h>
#include <dirent.h>
#include <sys/utsname.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define SYSINFO_HAVE_CPUID 1
#endif

/* ── small /proc and /sys readers ────────────────────────────────────── */

static int read_int_file(const char *path, int def)
{
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

static void copy_trim(char *dst, size_t len, const char *src)
{
    while (*src && isspace((unsigned char)*src)) src++;
    snprintf(dst, len, "%s", src);
    size_t n = strlen(dst);
    while (n > 0 && isspace((unsigned char)dst[n - 1])) dst[--n] = '\0';
}

static int count_numa_nodes(void)
{
    DIR *d = opendir("/sys/devices/system/node");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "node", 4) == 0 && isdigit((unsigned char)e->d_name[4]))
            n++;
    }
    closedir(d);
    return n > 0 ? n : -1;
}

static int read_procs_running(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    int v = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "procs_running %d", &v) == 1) break;
    }
    fclose(f);
    return v;
}

static double read_loadavg1(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1.0;
    double v = -1.0;
    if (fscanf(f, "%lf", &v) != 1) v = -1.0;
    fclose(f);
    return v;
}

/* ── CPUID ───────────────────────────────────────────────────────────── */

void sysinfo_classify_uarch(const char *vendor, int family, int model,
                            char *uarch, size_t uarch_len,
                            char *name,  size_t name_len)
{
    const char *u = "", *nm = "";

    if (strcmp(vendor, "AuthenticAMD") == 0) {
        if (family == 0x17) {
            if (model < 0x30) { u = "zen";  nm = "Naples / Zen(+)"; }
            else              { u = "zen2"; nm = "Rome / Zen 2"; }
        } else if (family == 0x19) {
            if      (model <= 0x0F)                  { u = "zen3"; nm = "Milan"; }
            else if (model <= 0x1F)                  { u = "zen4"; nm = "Genoa"; }
            else if (model <= 0x5F)                  { u = "zen3"; nm = "Zen 3 client"; }
            else if (model <= 0x7F)                  { u = "zen4"; nm = "Zen 4 client"; }
            else if (model >= 0xA0 && model <= 0xAF) { u = "zen4"; nm = "Bergamo / Siena"; }
        } else if (family == 0x1A) {
            /* Zen 5 and Zen 6 share family 1Ah. Ranges as in Linux
             * arch/x86/kernel/cpu/amd.c bsp_init_amd() (July 2026, after
             * "Carve out a Zen5 models range"):
             *   Zen 5: 00-2F, 40-4F, 60-7F, D0-D7
             *   Zen 6: 50-5F, 80-AF, C0-CF, D8-EF
             * Zen 6 client models named in AMD's EPP patches: 80, 81, 84,
             * 85, E0; other Zen 6 models are labelled Venice. Zen 6 core
             * counters use the Zen 5 tables, checked against perf on an
             * EPYC 9996 (counters.c); node memory counters are separate
             * (nodemon.c). */
            if      (model <= 0x1F)                  { u = "zen5"; nm = "Turin"; }
            else if (model <= 0x2F ||
                     (model >= 0x40 && model <= 0x4F) ||
                     (model >= 0x60 && model <= 0x7F) ||
                     (model >= 0xD0 && model <= 0xD7)) { u = "zen5"; nm = "Zen 5 client"; }
            else if (model == 0x80 || model == 0x81 || model == 0x84 ||
                     model == 0x85 || model == 0xE0)   { u = "zen6"; nm = "Zen 6 client"; }
            else if ((model >= 0x50 && model <= 0x5F) ||
                     (model >= 0x80 && model <= 0xAF) ||
                     (model >= 0xC0 && model <= 0xCF) ||
                     (model >= 0xD8 && model <= 0xEF)) { u = "zen6"; nm = "Venice"; }
        }
    } else if (strcmp(vendor, "GenuineIntel") == 0 && family == 6) {
        switch (model) {
        case 0x55:              u = "skylake_sp";      nm = "Skylake/Cascade Lake SP"; break;
        case 0x6A: case 0x6C:   u = "icelake_sp";      nm = "Ice Lake SP";             break;
        case 0x8F:              u = "sapphire_rapids"; nm = "Sapphire Rapids";         break;
        case 0xCF:              u = "emerald_rapids";  nm = "Emerald Rapids";          break;
        case 0xAD: case 0xAE:   u = "granite_rapids";  nm = "Granite Rapids";          break;
        case 0xAF:              u = "sierra_forest";   nm = "Sierra Forest";           break;
        default: break;
        }
    }
    snprintf(uarch, uarch_len, "%s", u);
    snprintf(name,  name_len,  "%s", nm);
}

static void collect_cpuid(sysinfo_node_t *n)
{
    n->cpu_family = n->cpu_model = n->cpu_stepping = -1;
    n->cpu_vendor[0] = n->cpu_brand[0] = n->uarch[0] = n->uarch_name[0] = '\0';

#ifdef SYSINFO_HAVE_CPUID
    unsigned a, b, c, d;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return;
    unsigned vend[3] = { b, d, c };
    memcpy(n->cpu_vendor, vend, 12);
    n->cpu_vendor[12] = '\0';

    if (__get_cpuid(1, &a, &b, &c, &d)) {
        int base_fam  = (a >> 8)  & 0xF;
        int base_mod  = (a >> 4)  & 0xF;
        int ext_fam   = (a >> 20) & 0xFF;
        int ext_mod   = (a >> 16) & 0xF;
        n->cpu_stepping = a & 0xF;
        n->cpu_family   = base_fam + (base_fam == 0xF ? ext_fam : 0);
        n->cpu_model    = (base_fam == 0x6 || base_fam == 0xF)
                          ? (ext_mod << 4) + base_mod : base_mod;
    }

    unsigned max_ext = __get_cpuid_max(0x80000000, NULL);
    if (max_ext >= 0x80000004) {
        unsigned brand[12];
        for (unsigned i = 0; i < 3; i++)
            __get_cpuid(0x80000002 + i, &brand[i * 4], &brand[i * 4 + 1],
                        &brand[i * 4 + 2], &brand[i * 4 + 3]);
        char tmp[49];
        memcpy(tmp, brand, 48);
        tmp[48] = '\0';
        copy_trim(n->cpu_brand, sizeof(n->cpu_brand), tmp);
    }

    sysinfo_classify_uarch(n->cpu_vendor, n->cpu_family, n->cpu_model,
                           n->uarch, sizeof(n->uarch),
                           n->uarch_name, sizeof(n->uarch_name));
#endif
}

/* ── public ──────────────────────────────────────────────────────────── */

void sysinfo_collect_node(sysinfo_node_t *n)
{
    memset(n, 0, sizeof(*n));

    if (gethostname(n->hostname, sizeof(n->hostname)) != 0)
        n->hostname[0] = '\0';
    n->hostname[sizeof(n->hostname) - 1] = '\0';

    struct utsname u;
    if (uname(&u) == 0) snprintf(n->kernel, sizeof(n->kernel), "%.*s", (int)sizeof(n->kernel) - 1, u.release);

    collect_cpuid(n);

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    n->logical_cpus        = ncpu > 0 ? (int)ncpu : -1;
    n->numa_nodes          = count_numa_nodes();
    n->smt_active          = read_int_file("/sys/devices/system/cpu/smt/active", -1);
    n->perf_event_paranoid = read_int_file("/proc/sys/kernel/perf_event_paranoid",
                                           SYSINFO_PARANOID_UNKNOWN);
    n->loadavg1            = read_loadavg1();
    n->procs_running       = read_procs_running();
}

void sysinfo_collect_rank(sysinfo_rank_t *r)
{
    memset(r, 0, sizeof(*r));
    r->pid = (int)getpid();
    r->cpus_allowed = -1;
    r->cores_allowed = -1;

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return;

    r->cpus_allowed = CPU_COUNT(&set);

    /* Physical cores = distinct (package, core_id) pairs in the mask. With
     * SMT on, launchers usually bind a rank to a whole core (2 hardware
     * threads), so utilisation against hardware threads would read ~50%
     * for a fully busy single-threaded rank. */
    /* One sysfs read per core, not two per CPU: an unbound rank on a
     * 384-CPU node would otherwise read 768 files, and every rank on the
     * node does it at once (0.17 s at 192 ranks). Each CPU's
     * thread_siblings_list names its whole core; siblings are skipped. */
    {
        cpu_set_t seen;
        CPU_ZERO(&seen);
        int n = 0, ok = 1;
        for (int cpu = 0; cpu < CPU_SETSIZE && ok; cpu++) {
            if (!CPU_ISSET(cpu, &set) || CPU_ISSET(cpu, &seen)) continue;
            char path[96], buf[256];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
            FILE *f = fopen(path, "r");
            if (!f || !fgets(buf, sizeof(buf), f)) { if (f) fclose(f); ok = 0; break; }
            fclose(f);
            CPU_SET(cpu, &seen);
            for (char *s = buf; *s && *s != '\n'; ) {        /* "0,192" or "0-1" */
                char *e;
                long a = strtol(s, &e, 10), b = a;
                if (e == s) break;
                if (*e == '-') b = strtol(e + 1, &e, 10);
                for (long c = a; c <= b && c < CPU_SETSIZE; c++) CPU_SET((int)c, &seen);
                s = (*e == ',') ? e + 1 : e;
            }
            n++;
        }
        r->cores_allowed = ok ? n : -1;
    }

    /* Compact range list, e.g. "0-3,64-67". Truncated with "..." if it
     * doesn't fit — the count above stays exact either way. */
    char *p = r->cpus_allowed_list;
    size_t left = sizeof(r->cpus_allowed_list);
    int first = 1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &set)) continue;
        int end = cpu;
        while (end + 1 < CPU_SETSIZE && CPU_ISSET(end + 1, &set)) end++;
        char item[32];
        if (end == cpu) snprintf(item, sizeof(item), "%s%d", first ? "" : ",", cpu);
        else            snprintf(item, sizeof(item), "%s%d-%d", first ? "" : ",", cpu, end);
        size_t need = strlen(item);
        if (need + 4 >= left) { snprintf(p, left, "..."); break; }
        memcpy(p, item, need + 1);
        p += need; left -= need;
        first = 0;
        cpu = end;
    }
}
