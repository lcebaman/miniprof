/*
 * symbolizer.c — resolve raw PCs to human-readable symbols
 *
 * Resolution strategy, applied in order until a name is found:
 *
 *   1. dladdr()         — fast, works for shared-library globals and
 *                         PIE main-executable globals, but CANNOT see
 *                         static/local symbols (only reads .dynsym).
 *
 *   2. addr2line        — subprocess, reads .symtab + DWARF for
 *                         file:line. Needs the correct link-time
 *                         address (runtime_pc − dli_fbase for PIE).
 *                         Can fail silently due to:
 *                           - wrong PIE base on some glibc versions
 *                           - GNU vs LLVM addr2line differences
 *                           - DWARF5 incompatibilities
 *                           - path aliasing on shared/network FS
 *
 *   3. nm (fallback)    — reads .symtab directly, no DWARF needed,
 *                         no address-translation issues (we compute
 *                         the correct link-time range from nm output
 *                         and /proc/self/maps). This is the same
 *                         proven mechanism used by mpa-check and
 *                         instrument.c's trace-mode filter.
 *                         Only gives function name, not file:line.
 *
 * Resolution 3 was added because real-world testing on production HPC
 * clusters showed addr2line silently returning "??" for every main-
 * executable PC (while libc/libmpi PCs resolved fine), leaving the
 * entire USER group as one undifferentiated "??" bucket — the worst
 * possible profiling outcome. The nm fallback guarantees at least
 * function-level attribution even when DWARF/PIE/addr2line interactions
 * fail on a particular system.
 *
 * All resolution happens at finalize time, never in the signal handler.
 */

#define _GNU_SOURCE
#include "symbolizer.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

/* ── globals ─────────────────────────────────────────────────────────────── */
static char g_exe_path[512] = {0};
static int  g_verbose = 0;

void symbolizer_init(void)
{
    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path)-1);
    if (n > 0) g_exe_path[n] = '\0';
    else        snprintf(g_exe_path, sizeof(g_exe_path), "unknown");

    const char *v = getenv("MINIPROF_VERBOSE");
    g_verbose = (v && atoi(v));
}

/* ── group classification ────────────────────────────────────────────────── */
static void classify_group(const char *dso, const char *name, char *out)
{
    #define HAS(s, sub) (strstr((s), (sub)) != NULL)

    if (!dso[0] || strcmp(dso, "??") == 0) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_OTHER);
    } else if (HAS(name, "sigprof_handler") || HAS(name, "profile_store_") ||
               HAS(name, "path_store_") ||
               HAS(name, "trace_store_") || HAS(name, "miniprof_") ||
               HAS(name, "symbolizer_") || HAS(name, "aggregator_") ||
               HAS(name, "instrument_") || HAS(name, "mpa_write")) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_PROFILER);
    } else if (HAS(dso, "libmpi") || HAS(dso, "mca_") || HAS(name, "MPI_") ||
        HAS(name, "PMPI_") || HAS(name, "ompi_") || HAS(name, "mca_")) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_MPI);
    } else if (HAS(dso, "libgomp") || HAS(dso, "libomp") ||
               HAS(name, "GOMP_") || HAS(name, "__kmp")) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_OMP);
    } else if (HAS(dso, "libc.so") || HAS(dso, "libc-") ||
               HAS(dso, "libm.so") || HAS(dso, "libpthread")) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_LIBC);
    } else if (HAS(dso, "ld-linux") || HAS(dso, "libdl") ||
               HAS(name, "__libc_start") || strcmp(name, "_start") == 0) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_SYS);
    } else if (HAS(dso, ".so") == 0) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_USER);
    } else {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_OTHER);
    }

    #undef HAS
}

/* ── dladdr resolution ───────────────────────────────────────────────────── */
static int resolve_dladdr(void *pc, miniprof_sym_t *out,
                          void **fbase_out, char *full_path_out)
{
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr(pc, &info)) return 0;

    const char *sym = info.dli_sname ? info.dli_sname : "??";
    snprintf(out->name, MINIPROF_SYMBOL_LEN, "%s", sym);

    const char *dso = info.dli_fname ? info.dli_fname : "??";
    const char *base = strrchr(dso, '/');
    snprintf(out->dso, MINIPROF_SYMBOL_LEN, "%s", base ? base+1 : dso);
    if (full_path_out)
        snprintf(full_path_out, MINIPROF_SYMBOL_LEN, "%s", dso);

    out->pc   = pc;
    out->file[0] = '\0';
    out->line    = 0;

    if (fbase_out) *fbase_out = info.dli_fbase;
    return 1;
}

/* ── addr2line batch resolution ──────────────────────────────────────────── */
#define ADDR2LINE_BATCH 64

static void resolve_addr2line_batch(miniprof_sym_t *syms, void **fbases,
                                    const char *dso_path, int n)
{
    if (n <= 0 || !dso_path || !dso_path[0]) return;

    /* First attempt: link-time addresses (runtime_pc − dli_fbase).
     * This is correct for PIE executables and shared libraries on
     * most systems. */
    char cmd[8192];
    int  pos = snprintf(cmd, sizeof(cmd),
                        "addr2line -e '%s' -f -C 2>/dev/null", dso_path);

    for (int i = 0; i < n && pos < (int)sizeof(cmd)-32; i++) {
        uintptr_t runtime_pc = (uintptr_t)syms[i].pc;
        uintptr_t base_addr  = fbases[i] ? (uintptr_t)fbases[i] : 0;
        uintptr_t link_pc    = runtime_pc - base_addr;
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " 0x%lx", (unsigned long)link_pc);
    }

    if (g_verbose) {
        int rank = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if (rank == 0)
            fprintf(stderr, "[miniprof] addr2line cmd (first %d chars): %.200s...\n",
                    pos, cmd);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char func[MINIPROF_SYMBOL_LEN];
    char fileline[MINIPROF_SYMBOL_LEN];
    int  n_resolved = 0;

    for (int i = 0; i < n; i++) {
        if (!fgets(func,     sizeof(func),     fp)) break;
        if (!fgets(fileline, sizeof(fileline), fp)) break;

        func    [strcspn(func,     "\n")] = '\0';
        fileline[strcspn(fileline, "\n")] = '\0';

        if (strcmp(func, "??") != 0) {
            snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%s", func);
            n_resolved++;
        }

        char *colon = strrchr(fileline, ':');
        if (colon) {
            *colon = '\0';
            const char *fbase = strrchr(fileline, '/');
            snprintf(syms[i].file, MINIPROF_SYMBOL_LEN,
                     "%s", fbase ? fbase+1 : fileline);
            syms[i].line = atoi(colon+1);
        }
    }
    pclose(fp);

    if (g_verbose) {
        int rank = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if (rank == 0)
            fprintf(stderr, "[miniprof] addr2line pass 1: %d/%d resolved "
                    "for %s\n", n_resolved, n, dso_path);
    }

    /* ── Second attempt: raw runtime addresses (no base subtraction).
     * Some addr2line versions (newer binutils, LLVM) auto-detect PIE
     * and apply the base correction themselves, meaning our manual
     * subtraction produces the WRONG (double-adjusted) address.
     * We only retry for PCs that are still "??" after the first pass. */
    int n_still_unknown = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(syms[i].name, "??") == 0) n_still_unknown++;

    if (n_still_unknown > 0) {
        pos = snprintf(cmd, sizeof(cmd),
                       "addr2line -e '%s' -f -C 2>/dev/null", dso_path);
        /* only the unknowns, but we need to track which indices they map to */
        int retry_idx[ADDR2LINE_BATCH];
        int retry_n = 0;
        for (int i = 0; i < n && retry_n < ADDR2LINE_BATCH; i++) {
            if (strcmp(syms[i].name, "??") == 0) {
                uintptr_t raw_pc = (uintptr_t)syms[i].pc;
                pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                                " 0x%lx", (unsigned long)raw_pc);
                retry_idx[retry_n++] = i;
            }
        }

        fp = popen(cmd, "r");
        if (fp) {
            int pass2_resolved = 0;
            for (int k = 0; k < retry_n; k++) {
                if (!fgets(func,     sizeof(func),     fp)) break;
                if (!fgets(fileline, sizeof(fileline), fp)) break;
                func    [strcspn(func,     "\n")] = '\0';
                fileline[strcspn(fileline, "\n")] = '\0';

                int i = retry_idx[k];
                if (strcmp(func, "??") != 0) {
                    snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%s", func);
                    pass2_resolved++;
                }
                char *colon = strrchr(fileline, ':');
                if (colon) {
                    *colon = '\0';
                    const char *fbase = strrchr(fileline, '/');
                    snprintf(syms[i].file, MINIPROF_SYMBOL_LEN,
                             "%s", fbase ? fbase+1 : fileline);
                    syms[i].line = atoi(colon+1);
                }
            }
            pclose(fp);

            if (g_verbose) {
                int rank = 0;
                MPI_Comm_rank(MPI_COMM_WORLD, &rank);
                if (rank == 0)
                    fprintf(stderr, "[miniprof] addr2line pass 2 (raw PCs): "
                            "%d/%d resolved\n", pass2_resolved, retry_n);
            }
        }
    }
}

/* ── nm-based fallback for the main executable ───────────────────────────── *
 *
 * When addr2line fails (returns "??" for all PCs), this function
 * provides function-level attribution using nm — the same proven
 * mechanism used by mpa-check and instrument.c's trace-mode filter.
 *
 * It reads the full .symtab of the executable via `nm`, loads the
 * text-segment base from /proc/self/maps, and matches each unresolved
 * PC to the tightest enclosing symbol by address range.
 *
 * This gives function names but NOT file:line info (that requires
 * DWARF, which is what addr2line does — if addr2line failed, we
 * accept the trade-off of names-only rather than nothing).
 */
#define NM_MAX_SYMS 4096

typedef struct {
    unsigned long addr;
    unsigned long size;     /* 0 if not known; estimated from next symbol */
    char          name[128];
    char          type;     /* 't' or 'T' */
} nm_sym_t;

static int nm_sym_cmp(const void *a, const void *b)
{
    unsigned long aa = ((const nm_sym_t *)a)->addr;
    unsigned long bb = ((const nm_sym_t *)b)->addr;
    return (aa > bb) - (aa < bb);
}

static uintptr_t find_exe_load_base(const char *exe_path)
{
    /* Find the load base of the main executable from /proc/self/maps.
     * For a PIE binary, the first r-xp mapping matching our exe path
     * is the .text segment; its start address minus its file offset
     * gives the ASLR slide base. */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    /* extract just the basename for matching, since maps may show the
       full realpath while exe_path might be a relative or symlinked path */
    const char *exe_base = strrchr(exe_path, '/');
    exe_base = exe_base ? exe_base + 1 : exe_path;

    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, exe_base)) continue;
        if (!strstr(line, "r-xp") && !strstr(line, "r--xp")) continue;

        unsigned long start, end, offset;
        if (sscanf(line, "%lx-%lx %*s %lx", &start, &end, &offset) == 3) {
            base = start - offset;
            break;
        }
    }
    fclose(f);
    return base;
}

static void resolve_nm_fallback(miniprof_sym_t *syms, int n,
                                const char *exe_path)
{
    /* only apply to PCs that are still "??" and belong to the main exe */
    int n_unknown = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(syms[i].name, "??") == 0 &&
            strstr(syms[i].dso, ".so") == NULL)
            n_unknown++;

    if (n_unknown == 0) return;

    /* find load base */
    uintptr_t load_base = find_exe_load_base(exe_path);
    if (load_base == 0) {
        if (g_verbose)
            fprintf(stderr, "[miniprof] nm fallback: failed to find load base "
                    "for '%s' in /proc/self/maps\n", exe_path);
        return;
    }

    /* read nm output */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "nm -n '%s' 2>/dev/null", exe_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    nm_sym_t *nm_syms = calloc(NM_MAX_SYMS, sizeof(nm_sym_t));
    int nm_n = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && nm_n < NM_MAX_SYMS) {
        unsigned long addr;
        char type;
        char name[128];
        if (sscanf(line, "%lx %c %127s", &addr, &type, name) != 3) continue;
        if (type != 't' && type != 'T') continue;  /* text symbols only */
        nm_syms[nm_n].addr = addr;
        nm_syms[nm_n].type = type;
        snprintf(nm_syms[nm_n].name, sizeof(nm_syms[nm_n].name), "%s", name);
        nm_n++;
    }
    pclose(fp);

    if (nm_n == 0) {
        if (g_verbose)
            fprintf(stderr, "[miniprof] nm fallback: nm returned 0 text "
                    "symbols for '%s' (stripped binary?)\n", exe_path);
        free(nm_syms);
        return;
    }

    /* nm -n already sorts by address, but be safe */
    qsort(nm_syms, nm_n, sizeof(nm_sym_t), nm_sym_cmp);

    /* fill in sizes from gaps */
    for (int i = 0; i < nm_n - 1; i++)
        nm_syms[i].size = nm_syms[i+1].addr - nm_syms[i].addr;
    nm_syms[nm_n-1].size = 0x10000;  /* generous for last symbol */

    /* match each unresolved PC */
    int resolved = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(syms[i].name, "??") != 0) continue;
        if (strstr(syms[i].dso, ".so") != NULL) continue;

        /* convert runtime PC to link-time address */
        uintptr_t link_addr = (uintptr_t)syms[i].pc - load_base;

        /* binary search for the enclosing symbol */
        int lo = 0, hi = nm_n - 1, best = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (nm_syms[mid].addr <= link_addr) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (best >= 0 && link_addr < nm_syms[best].addr + nm_syms[best].size) {
            snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%s",
                     nm_syms[best].name);
            resolved++;
        }
    }

    if (g_verbose) {
        int rank = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if (rank == 0)
            fprintf(stderr, "[miniprof] nm fallback: %d/%d resolved "
                    "(%d text symbols from nm, load_base=0x%lx)\n",
                    resolved, n_unknown, nm_n, (unsigned long)load_base);
    }
    free(nm_syms);
}

/* ── quick name resolution (trace mode only) ─────────────────────────────── */
int symbolizer_quick_name(void *pc, char *out, int outlen)
{
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr(pc, &info) || !info.dli_sname) return 0;
    snprintf(out, outlen, "%s", info.dli_sname);
    return 1;
}

/* ── main resolve entry point ────────────────────────────────────────────── */
void symbolizer_resolve(miniprof_sym_t *syms, int n, int use_addr2line)
{
    void **fbases    = calloc(n, sizeof(void *));
    char (*dsopaths)[MINIPROF_SYMBOL_LEN] = calloc(n, MINIPROF_SYMBOL_LEN);

    /* pass 1: dladdr */
    for (int i = 0; i < n; i++) {
        if (!resolve_dladdr(syms[i].pc, &syms[i], &fbases[i], dsopaths[i])) {
            snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%p", syms[i].pc);
            snprintf(syms[i].dso,  MINIPROF_SYMBOL_LEN, "??");
            fbases[i] = NULL;
            dsopaths[i][0] = '\0';
        }
    }

    /* pass 2: addr2line (grouped by DSO) */
    if (use_addr2line) {
        int *done = calloc(n, sizeof(int));
        for (int i = 0; i < n; i++) {
            if (done[i] || !dsopaths[i][0]) continue;

            int idxs[256];
            int cnt = 0;
            for (int j = i; j < n && cnt < 256; j++) {
                if (!done[j] && strcmp(dsopaths[j], dsopaths[i]) == 0)
                    idxs[cnt++] = j;
            }

            miniprof_sym_t tmp_syms[256];
            void          *tmp_fbase[256];
            for (int k = 0; k < cnt; k++) {
                tmp_syms[k]  = syms[idxs[k]];
                tmp_fbase[k] = fbases[idxs[k]];
            }

            for (int b = 0; b < cnt; b += ADDR2LINE_BATCH) {
                int batch = cnt - b;
                if (batch > ADDR2LINE_BATCH) batch = ADDR2LINE_BATCH;
                resolve_addr2line_batch(tmp_syms + b, tmp_fbase + b,
                                        dsopaths[i], batch);
            }

            for (int k = 0; k < cnt; k++) {
                syms[idxs[k]] = tmp_syms[k];
                done[idxs[k]] = 1;
            }
        }
        free(done);
    }

    /* pass 3: nm fallback for anything still "??" in the main executable */
    resolve_nm_fallback(syms, n, g_exe_path);

    /* classify into groups */
    for (int i = 0; i < n; i++)
        classify_group(syms[i].dso, syms[i].name, syms[i].group);

    free(fbases);
    free(dsopaths);
}
