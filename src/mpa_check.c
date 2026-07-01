/*
 * mpa_check.c — standalone build-time validator for .mpa trace lists
 *
 * Run this right after building the instrumented (+trace) binary and
 * before submitting a (possibly expensive, multi-node) tracing job.
 * It catches exactly the failure mode that would otherwise only show up
 * as a runtime warning deep inside an MPI job's stderr: a typo'd
 * function name, a function the compiler inlined away, or a .mpa file
 * generated against a different binary.
 *
 * Usage:
 *   ./mpa-check <binary> <mpa-file>
 *
 * Exit status:
 *   0  — every named function resolved successfully
 *   1  — one or more functions failed to resolve (details printed)
 *   2  — usage error / file not found / nm not available
 *
 * This deliberately reuses the exact same resolution method as the
 * runtime instrumentation filter (nm against the binary's .symtab, not
 * dladdr — see instrument.c's file header for why), so a clean result
 * here is a real guarantee, not just a heuristic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAMES   512
#define MAX_NAME_LEN 128

typedef struct {
    char name[MAX_NAME_LEN];
    int  resolved;
    char type;          /* 't'=local text, 'T'=global text, 0=unresolved */
    unsigned long addr;
} entry_t;

static entry_t g_entries[MAX_NAMES];
static int     g_n_entries = 0;

static int load_mpa_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "mpa-check: cannot open '%s': ", path);
        perror("");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) && g_n_entries < MAX_NAMES) {
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

        snprintf(g_entries[g_n_entries].name, MAX_NAME_LEN, "%s", start);
        g_entries[g_n_entries].resolved = 0;
        g_entries[g_n_entries].type     = 0;
        g_entries[g_n_entries].addr     = 0;
        g_n_entries++;
    }
    fclose(f);
    return 1;
}

static int resolve_against_binary(const char *binary)
{
    char cmd[600];
    int rc = snprintf(cmd, sizeof(cmd), "nm '%s' 2>/dev/null", binary);
    if (rc < 0 || rc >= (int)sizeof(cmd)) {
        fprintf(stderr, "mpa-check: binary path too long\n");
        return 0;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "mpa-check: failed to run nm — is it installed?\n");
        return 0;
    }

    char line[512];
    int any_output = 0;
    while (fgets(line, sizeof(line), fp)) {
        any_output = 1;
        unsigned long addr;
        char type;
        char name[MAX_NAME_LEN];
        if (sscanf(line, "%lx %c %127s", &addr, &type, name) != 3) continue;
        if (type != 't' && type != 'T') continue;

        for (int i = 0; i < g_n_entries; i++) {
            if (!g_entries[i].resolved &&
                strcmp(g_entries[i].name, name) == 0) {
                g_entries[i].resolved = 1;
                g_entries[i].type     = type;
                g_entries[i].addr     = addr;
            }
        }
    }
    int status = pclose(fp);

    if (!any_output) {
        fprintf(stderr, "mpa-check: nm produced no output for '%s' — "
                       "check the binary exists and is not stripped\n",
                binary);
        return 0;
    }
    (void)status;
    return 1;
}

static void check_instrumentation(const char *binary)
{
    /*
     * Sanity check: was this binary actually built with
     * -finstrument-functions?
     *
     * It is NOT enough to check whether __cyg_profile_func_enter exists
     * as a symbol in the binary — it always will, because that symbol is
     * defined in libminiprof.a's instrument.c, which is unconditionally
     * linked into every binary that links the library, instrumented or
     * not. The real signal is whether anything actually *calls* it: with
     * -finstrument-functions, the compiler inserts a call instruction at
     * the top of every compiled function; without that flag, zero call
     * sites exist anywhere in the binary even though the symbol is
     * present and resolvable. We disassemble with objdump and count call
     * sites referencing it.
     */
    char cmd[700];
    snprintf(cmd, sizeof(cmd),
            "objdump -d '%s' 2>/dev/null | "
            "grep -c 'call.*__cyg_profile_func_enter'",
            binary);
    FILE *fp = popen(cmd, "r");
    int call_sites = 0;
    if (fp) {
        if (fscanf(fp, "%d", &call_sites) != 1) call_sites = 0;
        pclose(fp);
    }

    if (call_sites == 0) {
        fprintf(stderr,
            "\nmpa-check: WARNING — '%s' has zero call sites to "
            "__cyg_profile_func_enter.\n"
            "  This binary was NOT built with -finstrument-functions "
            "(the symbol exists because\n"
            "  it's linked in from libminiprof.a, but nothing calls it "
            "in this build).\n"
            "  Trace mode will record zero user-function calls even "
            "though mpa-check may\n"
            "  report names as resolved above (a resolved name only "
            "means the symbol's\n"
            "  address was found — it does not mean entry/exit hooks "
            "will actually fire).\n"
            "  Rebuild with: mpicc ... -finstrument-functions ... \n"
            "  (the Makefile's `example-trace` / `make example-trace` "
            "target does this for you)\n", binary);
    } else {
        printf("\nmpa-check: confirmed %d instrumentation call site(s) "
               "in '%s' — this binary was built with "
               "-finstrument-functions.\n", call_sites, binary);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <binary> <mpa-file>\n", argv[0]);
        fprintf(stderr, "  Validates that every function name in <mpa-file> "
                       "resolves to a real\n  symbol in <binary>, before "
                       "submitting an expensive tracing job.\n");
        return 2;
    }

    const char *binary   = argv[1];
    const char *mpa_path = argv[2];

    if (!load_mpa_file(mpa_path)) return 2;
    if (g_n_entries == 0) {
        fprintf(stderr, "mpa-check: '%s' contains no function names "
                       "(only comments/blank lines?)\n", mpa_path);
        return 2;
    }

    if (!resolve_against_binary(binary)) return 2;

    int n_ok = 0, n_fail = 0;
    printf("mpa-check: validating %d function(s) from '%s' against '%s'\n\n",
           g_n_entries, mpa_path, binary);

    for (int i = 0; i < g_n_entries; i++) {
        if (g_entries[i].resolved) {
            printf("  OK    %-40s  0x%08lx  %s\n",
                   g_entries[i].name, g_entries[i].addr,
                   g_entries[i].type == 't' ? "(local/static)" : "(global)");
            n_ok++;
        } else {
            printf("  FAIL  %-40s  (not found in binary)\n",
                   g_entries[i].name);
            n_fail++;
        }
    }

    printf("\n%d resolved, %d failed, out of %d total\n",
           n_ok, n_fail, g_n_entries);

    check_instrumentation(binary);

    if (n_fail > 0) {
        fprintf(stderr,
            "\nmpa-check: FAILED — %d function(s) will not be traced.\n"
            "  Common causes:\n"
            "    1. Typo in the .mpa file\n"
            "    2. Function was inlined away or optimized out\n"
            "       (try -O0 or __attribute__((noinline)) on that function)\n"
            "    3. .mpa file was generated against a different binary\n"
            "    4. Binary is stripped (rebuild without -s / strip)\n", n_fail);
        return 1;
    }

    printf("\nmpa-check: all functions resolved successfully.\n");
    return 0;
}
