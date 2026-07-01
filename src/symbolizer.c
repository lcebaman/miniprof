/*
 * symbolizer.c — resolve raw PCs to human-readable symbols
 *
 * Strategy (in order of preference):
 *   1. dladdr()         — fast, works for shared libs and PIE executables
 *   2. addr2line        — subprocess call, gives file:line from DWARF
 *
 * All resolution happens at finalize time, never in the signal handler.
 */

#define _GNU_SOURCE
#include "symbolizer.h"
#include "../include/miniprof.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

/* Path to our own executable, read from /proc/self/exe */
static char g_exe_path[512] = {0};

void symbolizer_init(void)
{
    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path)-1);
    if (n > 0) g_exe_path[n] = '\0';
    else        snprintf(g_exe_path, sizeof(g_exe_path), "unknown");
}

/* ── group classification ────────────────────────────────────────────────── */
/*
 * Classify a symbol into a CrayPat-style group based on its DSO and,
 * as a fallback, its symbol name prefix. Order matters: more specific
 * checks first.
 */
static void classify_group(const char *dso, const char *name, char *out)
{
    /* helper: case-sensitive substring match */
    #define HAS(s, sub) (strstr((s), (sub)) != NULL)

    if (!dso[0] || strcmp(dso, "??") == 0) {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_OTHER);
    } else if (HAS(name, "sigprof_handler") || HAS(name, "profile_store_") ||
               HAS(name, "trace_store_") || HAS(name, "miniprof_") ||
               HAS(name, "symbolizer_") || HAS(name, "aggregator_") ||
               HAS(name, "instrument_") || HAS(name, "mpa_write")) {
        /*
         * miniprof's own internal functions. Since the sampler now
         * correctly records the true interrupted-instruction leaf (see
         * sampler.c's fix), a small fraction of samples legitimately
         * land inside sigprof_handler's own epilogue (the few
         * instructions executing right as the signal is delivered, or
         * right after backtrace() returns and before the handler exits)
         * — this is real, but not useful to show mixed in with the
         * user's own USER-group code, so it gets its own bucket.
         */
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
        /* looks like the main executable (no .so suffix) → USER code */
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_USER);
    } else {
        snprintf(out, MINIPROF_GROUP_LEN, "%s", MINIPROF_GROUP_OTHER);
    }

    #undef HAS
}


static int resolve_dladdr(void *pc, miniprof_sym_t *out,
                          void **fbase_out, char *full_path_out)
{
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr(pc, &info)) return 0;

    /* symbol name */
    const char *sym = info.dli_sname ? info.dli_sname : "??";
    snprintf(out->name, MINIPROF_SYMBOL_LEN, "%s", sym);

    /* DSO/exe base name (for display) + full path (for addr2line -e) */
    const char *dso = info.dli_fname ? info.dli_fname : "??";
    const char *base = strrchr(dso, '/');
    snprintf(out->dso, MINIPROF_SYMBOL_LEN, "%s", base ? base+1 : dso);
    if (full_path_out)
        snprintf(full_path_out, MINIPROF_SYMBOL_LEN, "%s", dso);

    out->pc   = pc;
    out->file[0] = '\0';
    out->line    = 0;

    /* module load base — needed to convert a runtime PC (PIE-relocated)
       back to the link-time address that addr2line expects */
    if (fbase_out) *fbase_out = info.dli_fbase;
    return 1;
}

/* ── addr2line resolution (subprocess) ───────────────────────────────────── */
/*
 * Batches up to N addresses into a single addr2line invocation to keep
 * fork overhead low.  Returns file:line strings in the same order.
 */
#define ADDR2LINE_BATCH 64

static void resolve_addr2line_batch(miniprof_sym_t *syms, void **fbases,
                                    const char *dso_path, int n)
{
    if (n <= 0 || !dso_path || !dso_path[0]) return;

    /* Build command: addr2line -e <dso> -f -C <addr1> <addr2> ...
     * Addresses must be link-time (i.e. relative to the module's own
     * load base), not raw runtime PCs — required for PIE executables
     * and shared libraries, since addr2line reads the unrelocated
     * symbol/DWARF tables straight from the ELF file. */
    char cmd[8192];
    int  pos = snprintf(cmd, sizeof(cmd),
                        "addr2line -e '%s' -f -C 2>/dev/null", dso_path);

    for (int i = 0; i < n && pos < (int)sizeof(cmd)-32; i++) {
        uintptr_t runtime_pc = (uintptr_t)syms[i].pc;
        uintptr_t base       = fbases[i] ? (uintptr_t)fbases[i] : 0;
        uintptr_t link_pc    = runtime_pc - base;
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
                        " 0x%lx", (unsigned long)link_pc);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    /* addr2line outputs two lines per address: function\nfile:line */
    char func[MINIPROF_SYMBOL_LEN];
    char fileline[MINIPROF_SYMBOL_LEN];

    for (int i = 0; i < n; i++) {
        if (!fgets(func,     sizeof(func),     fp)) break;
        if (!fgets(fileline, sizeof(fileline), fp)) break;

        /* strip newlines */
        func    [strcspn(func,     "\n")] = '\0';
        fileline[strcspn(fileline, "\n")] = '\0';

        /* use demangled name if addr2line gave something useful */
        if (strcmp(func, "??") != 0)
            snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%s", func);

        /* parse file:line */
        char *colon = strrchr(fileline, ':');
        if (colon) {
            *colon = '\0';
            const char *base = strrchr(fileline, '/');
            snprintf(syms[i].file, MINIPROF_SYMBOL_LEN,
                     "%s", base ? base+1 : fileline);
            syms[i].line = atoi(colon+1);
        }
    }
    pclose(fp);
}

/* ── public interface ────────────────────────────────────────────────────── */
/*
 * Quick name-only resolution for trace mode, where we only have a
 * handful of distinct function PCs (the .mpa-selected set) and just
 * need the symbol name, not file:line or group classification.
 * Returns 1 on success, 0 if dladdr couldn't resolve a name.
 */
int symbolizer_quick_name(void *pc, char *out, int outlen)
{
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr(pc, &info) || !info.dli_sname) return 0;
    snprintf(out, outlen, "%s", info.dli_sname);
    return 1;
}


/*
 * Resolve an array of PCs.  First pass: dladdr (fast).
 *                            Second pass: addr2line in batches (slow, optional).
 */
void symbolizer_resolve(miniprof_sym_t *syms, int n, int use_addr2line)
{
    void **fbases    = calloc(n, sizeof(void *));
    char (*dsopaths)[MINIPROF_SYMBOL_LEN] = calloc(n, MINIPROF_SYMBOL_LEN);

    for (int i = 0; i < n; i++) {
        if (!resolve_dladdr(syms[i].pc, &syms[i], &fbases[i], dsopaths[i])) {
            snprintf(syms[i].name, MINIPROF_SYMBOL_LEN, "%p", syms[i].pc);
            snprintf(syms[i].dso,  MINIPROF_SYMBOL_LEN, "??");
            fbases[i] = NULL;
            dsopaths[i][0] = '\0';
        }
    }

    if (use_addr2line) {
        /*
         * Group PCs by their resolved DSO full path, then batch-call
         * addr2line once per group (once per shared object actually hit
         * by samples — typically just the main exe + libmpi + libc).
         * This fixes two bugs at once:
         *   1. PCs from libmpi.so/libc.so were being looked up against
         *      our own executable's DWARF table (always failed/wrong).
         *   2. Runtime (relocated) addresses were passed straight to
         *      addr2line, which expects link-time addresses for PIE.
         */
        int *done = calloc(n, sizeof(int));
        for (int i = 0; i < n; i++) {
            if (done[i] || !dsopaths[i][0]) continue;

            /* gather all indices sharing this DSO path */
            int idxs[256];
            int cnt = 0;
            for (int j = i; j < n && cnt < 256; j++) {
                if (!done[j] && strcmp(dsopaths[j], dsopaths[i]) == 0) {
                    idxs[cnt++] = j;
                }
            }

            /* build contiguous scratch arrays for this DSO group */
            miniprof_sym_t tmp_syms[256];
            void          *tmp_fbase[256];
            for (int k = 0; k < cnt; k++) {
                tmp_syms[k]  = syms[idxs[k]];
                tmp_fbase[k] = fbases[idxs[k]];
            }

            /* batch within this DSO group (addr2line cmdline length limit) */
            for (int base = 0; base < cnt; base += ADDR2LINE_BATCH) {
                int batch = cnt - base;
                if (batch > ADDR2LINE_BATCH) batch = ADDR2LINE_BATCH;
                resolve_addr2line_batch(tmp_syms + base, tmp_fbase + base,
                                        dsopaths[i], batch);
            }

            /* scatter results back and mark done */
            for (int k = 0; k < cnt; k++) {
                syms[idxs[k]] = tmp_syms[k];
                done[idxs[k]] = 1;
            }
        }
        free(done);
    }

    /* classify into USER/MPI/OMP/LIBC/SYSTEM/ETC groups */
    for (int i = 0; i < n; i++)
        classify_group(syms[i].dso, syms[i].name, syms[i].group);

    free(fbases);
    free(dsopaths);
}
