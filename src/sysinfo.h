#ifndef MINIPROF_SYSINFO_H
#define MINIPROF_SYSINFO_H

/*
 * sysinfo.h — node and rank metadata for snapshot mode
 *
 * Everything here is cheap (CPUID + a handful of small /proc and /sys
 * reads) and collected once at init, so a snapshot JSON always records
 * *what* it was measured on — CPU microarchitecture, SMT, NUMA layout,
 * counter privileges, affinity — before any metric is interpreted.
 * Later phases key their per-CPU event tables off `cpu_vendor` +
 * `cpu_family` + `cpu_model`, so those fields are the raw CPUID values,
 * with `uarch` only a human-readable label on top.
 *
 * Fields that can't be determined are left as -1 (ints) or "" (strings)
 * and emitted as null in JSON — never guessed.
 */

#include <stddef.h>

#define SYSINFO_STR 64

/* perf_event_paranoid can legitimately be -1, so "unknown" needs its own
 * sentinel rather than the -1 used for every other int field. */
#define SYSINFO_PARANOID_UNKNOWN (-1000)

typedef struct {
    char   hostname[SYSINFO_STR];
    char   kernel[SYSINFO_STR];        /* uname -r                              */
    char   cpu_vendor[16];             /* "AuthenticAMD", "GenuineIntel", ...   */
    char   cpu_brand[SYSINFO_STR];     /* CPUID brand string                    */
    char   uarch[32];                  /* "zen4", "granite_rapids", ... or ""   */
    char   uarch_name[32];             /* "Genoa / Bergamo / Siena", ... or ""  */
    int    cpu_family;                 /* display family (base + extended)      */
    int    cpu_model;                  /* display model  (base + extended)      */
    int    cpu_stepping;
    int    logical_cpus;               /* online CPUs on the node               */
    int    numa_nodes;                 /* /sys/devices/system/node/node*        */
    int    smt_active;                 /* /sys/devices/system/cpu/smt/active    */
    int    perf_event_paranoid;        /* /proc/sys/kernel/perf_event_paranoid  */
    double loadavg1;                   /* 1-min load average at init            */
    int    procs_running;              /* runnable tasks at init (/proc/stat)   */
} sysinfo_node_t;

typedef struct {
    int  pid;
    int  cpus_allowed;                 /* popcount of sched_getaffinity mask    */
    char cpus_allowed_list[128];       /* "0-3,64-67" style, truncated with ... */
} sysinfo_rank_t;

void sysinfo_collect_node(sysinfo_node_t *n);
void sysinfo_collect_rank(sysinfo_rank_t *r);

/* Map raw CPUID vendor/family/model to a uarch label. Exposed separately
 * so it can be unit-tested without the host CPU. Writes "" when unknown. */
void sysinfo_classify_uarch(const char *vendor, int family, int model,
                            char *uarch, size_t uarch_len,
                            char *name,  size_t name_len);

#endif /* MINIPROF_SYSINFO_H */
