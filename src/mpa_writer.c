/*
 * mpa_writer.c — write the miniprof Analysis (MPA) suggestions file
 *
 * After a sampling run, any USER function whose aggregate Samp% exceeds
 * MINIPROF_MPA_THRESHOLD is written to the .mpa file, one name per line.
 * This is the function list that trace mode reads via
 * MINIPROF_TRACE_FUNCS, mirroring CrayPat's `pat_build -O apa` output
 * and its downstream `pat_build -O myprogram+pat+...sdot.apa` rebuild.
 *
 * Why USER-only, not USER+MPI: trace mode's instrumentation filter
 * (instrument.c) resolves function names by running `nm` against the
 * *target executable* and matching addresses there — it has no visibility
 * into libmpi.so or its plugin .so files (mca_*.so), and never will,
 * since -finstrument-functions only instruments the file(s) it's applied
 * to at compile time. MPI call timing is already covered unconditionally
 * by the separate PMPI wrapper layer in mpi_trace.c, which requires no
 * .mpa entry at all. Including MPI-group function names here would just
 * produce permanent, confusing FAILs from mpa-check / the runtime
 * unresolved-symbol warning, for functions that are already being traced
 * through a completely different mechanism.
 *
 * File format (simple, human-editable):
 *
 *   # miniprof MPA suggestions — generated <timestamp>
 *   # Edit this file to add/remove functions before running trace mode.
 *   # One function name per line; '#' starts a comment.
 *   compute_stencil
 *   halo_exchange
 *   check_convergence
 */

#include "mpa_writer.h"
#include "../include/miniprof.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void mpa_write_suggestions(const char *path,
                           const char **names, const double *pcts,
                           const char **groups, int n,
                           double threshold_pct)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("miniprof: fopen .mpa"); return; }

    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(f, "# miniprof MPA suggestions — generated %s\n", tbuf);
    fprintf(f, "# Functions with Samp%% >= %.1f%% from the sampling pass.\n",
            threshold_pct);
    fprintf(f, "# Only USER-group functions are listed: MPI calls are\n");
    fprintf(f, "# always traced automatically (see mpi_trace.c) and don't\n");
    fprintf(f, "# need — and can't use — an entry here.\n");
    fprintf(f, "# Edit this file to add/remove functions, then run:\n");
    fprintf(f, "#   ./mpa-check <binary>+trace %s   (validate first!)\n", path);
    fprintf(f, "#   MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=%s "
               "mpirun -n <N> ./your_app+trace\n", path);
    fprintf(f, "# One function name per line; '#' starts a comment.\n#\n");
    fprintf(f, "# %-32s  %8s\n", "Function", "Samp%");

    int written = 0;
    for (int i = 0; i < n; i++) {
        /* USER only — see file header for why MPI-group names are
           deliberately excluded here */
        if (strcmp(groups[i], MINIPROF_GROUP_USER) != 0) continue;
        if (pcts[i] < threshold_pct) continue;

        fprintf(f, "%-34s  # %5.1f%%\n", names[i], pcts[i]);
        written++;
    }

    if (written == 0) {
        fprintf(f, "# (no USER functions exceeded the threshold — try\n"
                   "#  lowering MINIPROF_MPA_THRESHOLD, running a longer\n"
                   "#  sample, or check the sampling report's USER group\n"
                   "#  total — your code may be entirely MPI/library-bound)\n");
    }

    fclose(f);
    fprintf(stdout, "[miniprof] MPA suggestions written to: %s "
                    "(%d USER functions; MPI is always traced separately)\n",
            path, written);
}
