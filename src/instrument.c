/*
 * instrument.c — GCC/Clang -finstrument-functions hooks
 *
 * When the target is built with -finstrument-functions, the compiler
 * inserts a call to __cyg_profile_func_enter() at the start of every
 * function and __cyg_profile_func_exit() at every return. Without
 * filtering this is enormous overhead (every single function, including
 * tiny getters). We do not control the build step at function granularity,
 * so instead we filter at runtime against a small allow-list of addresses.
 *
 * IMPORTANT: the allow-list is built by resolving NAMES → ADDRESSES via
 * `nm` on the executable itself at startup, NOT via dladdr() on incoming
 * PCs. This matters because dladdr() only ever sees the *dynamic* symbol
 * table (.dynsym) — by ELF/ld design, that's the table meant for
 * resolving calls *across* shared-object boundaries. Local/static
 * functions (the common case for compute kernels in HPC codes, e.g.
 * `static void compute_stencil(...)`) are never placed in .dynsym
 * regardless of -rdynamic, so a dladdr-based filter would silently
 * match zero application functions. `nm` (and addr2line) read the
 * regular .symtab instead, which does contain local symbols as long as
 * the binary isn't stripped — exactly the same fix already applied to
 * the sampling-mode symbolizer for the same underlying reason.
 *
 * Once we have link-time addresses for the functions we care about, we
 * convert incoming runtime PCs back to link-time addresses using the
 * executable's own load base (read once from /proc/self/maps) and do a
 * simple linear/binary search — this stays correct under PIE/ASLR.
 */

#define _GNU_SOURCE
#include "trace_store.h"
#include "sampler_hwc.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* ── allow-list entry: one traced function's link-time address ──────────── */
typedef struct {
    char      name[MINIPROF_TRACE_MAX_NAME];
    uintptr_t link_addr;   /* address as printed by `nm` on the exe file   */
} trace_entry_t;

static trace_entry_t g_entries[MINIPROF_TRACE_MAX_FUNCS];
static int            g_n_entries = 0;
static int            g_loaded    = 0;
static int            g_instrument_enabled = 0;
static uintptr_t       g_exe_load_base = 0;
static char            g_exe_path[512] = {0};

/* ── find this executable's runtime load base from /proc/self/maps ──────── *
 * The first mapping for our own executable path gives us the base address
 * the loader placed it at — this is what link-time addresses (from `nm`)
 * need to be offset by to become comparable to runtime PCs, but ONLY for
 * a PIE (ET_DYN) binary. A non-PIE (ET_EXEC) binary loads at its fixed
 * link-time address — the same address `nm` already prints — so adding
 * this mapping base on top would double-count it and every comparison
 * in is_traced_pc() would silently fail to match (confirmed: this HPC
 * toolchain's mpicc produces ET_EXEC binaries by default, not PIE, so
 * this bug reliably fired here). See is_pie_executable() below.        */
static uintptr_t find_exe_load_base(const char *exe_path)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, exe_path)) {
            unsigned long start;
            if (sscanf(line, "%lx-", &start) == 1) {
                base = (uintptr_t)start;
                break;
            }
        }
    }
    fclose(f);
    return base;
}

/* ── is this executable PIE (ET_DYN) or fixed-address (ET_EXEC)? ─────────── *
 * Reads e_type directly from the ELF header (offset 16, 2 bytes,
 * little-endian on every platform this profiler targets) rather than
 * shelling out — this is a 64-byte read, no need for `readelf`/`file`.
 * Defaults to "PIE" (1) on any read failure, preserving the previous
 * (offsetting) behaviour rather than silently breaking address matching
 * in a new way if the file can't be inspected.                         */
static int is_pie_executable(const char *exe_path)
{
    FILE *f = fopen(exe_path, "rb");
    if (!f) return 1;
    unsigned char hdr[18];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < sizeof(hdr) || hdr[0] != 0x7f || hdr[1] != 'E' ||
        hdr[2] != 'L' || hdr[3] != 'F') {
        return 1;
    }
    uint16_t e_type = (uint16_t)(hdr[16] | (hdr[17] << 8));
    return e_type == 3; /* ET_DYN */
}

/* ── run `nm` once, parse out (name -> link-time address) for our names ─── */
static void resolve_trace_addresses(void)
{
    if (!g_exe_path[0] || g_n_entries == 0) return;

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "nm '%s' 2>/dev/null", g_exe_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* format: "<hexaddr> <type-char> <name>\n" (local syms have a
           lowercase type char, e.g. 't'; globals use uppercase, e.g. 'T') */
        unsigned long addr;
        char type;
        char name[MINIPROF_TRACE_MAX_NAME];
        if (sscanf(line, "%lx %c %127s", &addr, &type, name) != 3) continue;
        if (type != 't' && type != 'T') continue; /* only text/code symbols */

        for (int i = 0; i < g_n_entries; i++) {
            if (g_entries[i].link_addr == 0 &&
                strcmp(g_entries[i].name, name) == 0) {
                g_entries[i].link_addr = (uintptr_t)addr;
                break;
            }
        }
    }
    pclose(fp);
}

/* ── public: called once from miniprof_init() in trace mode ─────────────── */
void instrument_load_trace_list(const char *path)
{
    g_n_entries = 0;
    g_loaded = 1;

    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path)-1);
    if (n > 0) g_exe_path[n] = '\0';
    else        g_exe_path[0] = '\0';

    FILE *f = fopen(path, "r");
    if (!f) {
        g_instrument_enabled = 0;
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) &&
           g_n_entries < MINIPROF_TRACE_MAX_FUNCS) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                          line[len-1] == ' '  || line[len-1] == '\t')) {
            line[--len] = '\0';
        }
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (start[0] == '\0') continue;

        snprintf(g_entries[g_n_entries].name, MINIPROF_TRACE_MAX_NAME, "%s", start);
        g_entries[g_n_entries].link_addr = 0;
        g_n_entries++;
    }
    fclose(f);

    if (g_n_entries == 0) { g_instrument_enabled = 0; return; }

    resolve_trace_addresses();
    int is_pie = is_pie_executable(g_exe_path);
    g_exe_load_base = is_pie ? find_exe_load_base(g_exe_path) : 0;
    /* 0 is a legitimate load base for a non-PIE binary (its link-time
       and runtime addresses are identical), but for a PIE binary it
       means find_exe_load_base() failed to find our own mapping. */
    int load_base_ok = is_pie ? (g_exe_load_base != 0) : 1;

    /* report + drop entries we failed to resolve (typo, inlined away,
       optimized out, wrong build, etc.) — silent failure here would
       otherwise produce a trace report missing functions with no
       indication why, which is worse than a noisy warning */
    int kept = 0;
    int n_unresolved = 0;
    for (int i = 0; i < g_n_entries; i++) {
        if (g_entries[i].link_addr != 0) {
            if (kept != i) g_entries[kept] = g_entries[i];
            kept++;
        } else {
            n_unresolved++;
        }
    }

    if (n_unresolved > 0) {
        int rank = 0;
        /* MPI_COMM_WORLD may not be valid yet if called before MPI_Init
           in some embedding scenarios; guard defensively */
        int mpi_initialized = 0;
        MPI_Initialized(&mpi_initialized);
        if (mpi_initialized) MPI_Comm_rank(MPI_COMM_WORLD, &rank);

        if (rank == 0) {
            fprintf(stderr,
                "[miniprof] WARNING: %d of %d functions in '%s' could not "
                "be resolved in this binary and will NOT be traced:\n",
                n_unresolved, g_n_entries, path);
            for (int i = 0; i < g_n_entries; i++) {
                if (g_entries[i].link_addr == 0) {
                    fprintf(stderr, "[miniprof]   - %s\n", g_entries[i].name);
                }
            }
            fprintf(stderr,
                "[miniprof] Common causes: (1) typo in the .mpa file, "
                "(2) function was inlined away or optimized out — try "
                "-O0 or __attribute__((noinline)), (3) tracing a "
                "different binary than the one .mpa was generated for, "
                "(4) binary is stripped (build with -g, don't strip).\n"
                "[miniprof] Run the standalone checker before submitting "
                "a job to catch this earlier:\n"
                "[miniprof]   ./mpa-check <binary> %s\n", path);
        }
    }

    g_n_entries = kept;

    g_instrument_enabled = (g_n_entries > 0 && load_base_ok);
    trace_store_init();

    /* Trace mode has no sampler_start() equivalent to do this — sample
     * mode's counter group is opened there instead. Only bother if
     * there's actually something to trace. */
    if (g_instrument_enabled) hwc_init();
}

/* ── public: reverse lookup used by miniprof.c when building the report ─── *
 * Returns 1 and fills `out` if pc matches a traced function's entry
 * address, 0 otherwise. Avoids relying on dladdr (see file header note).  */
int instrument_lookup_name(void *pc_void, char *out, int outlen)
{
    uintptr_t runtime_pc = (uintptr_t)pc_void;
    for (int i = 0; i < g_n_entries; i++) {
        if (runtime_pc == g_entries[i].link_addr + g_exe_load_base) {
            snprintf(out, outlen, "%s", g_entries[i].name);
            return 1;
        }
    }
    return 0;
}

/* ── fast filter: is this runtime PC the entry point of a traced function? */
static int is_traced_pc(void *pc_void)
{
    if (!g_instrument_enabled) return 0;
    uintptr_t runtime_pc = (uintptr_t)pc_void;

    /* GCC calls __cyg_profile_func_enter with this_fn = the function's
       own entry address (not relocated by call-site offsets), so a
       direct equality check against (link_addr + load_base) is correct
       — no need for range/binary search. */
    for (int i = 0; i < g_n_entries; i++) {
        if (runtime_pc == g_entries[i].link_addr + g_exe_load_base)
            return 1;
    }
    return 0;
}

/* ── compiler-inserted hooks ─────────────────────────────────────────────── *
 * no_instrument_function is essential: without it, GCC would instrument
 * these hooks themselves, causing infinite recursion.                      */
void __cyg_profile_func_enter(void *this_fn, void *call_site)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *this_fn, void *call_site)
    __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    (void)call_site;
    if (!g_loaded) return;
    if (!is_traced_pc(this_fn)) return;
    trace_store_enter(this_fn);
}

void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    (void)call_site;
    if (!g_loaded) return;
    if (!is_traced_pc(this_fn)) return;
    trace_store_exit(this_fn);
}
