/*
 * trace_aggregator.c — MPI_Gather trace profiles, write tracing report
 *
 * The report contains two summary tables:
 *
 *   Table: User Function Tracing (calls, inclusive/exclusive time)
 *   Table: MPI Call Tracing (calls, time, message bytes)
 *
 * Per-rank data is sent as raw bytes (MPI_BYTE), same approach as the
 * sampling aggregator, to avoid MPI derived-datatype construction.
 */

#include "trace_aggregator.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void hline(FILE *f, int w) { for (int i=0;i<w;i++) fputc('-',f); fputc('\n',f); }
static void dline(FILE *f, int w) { for (int i=0;i<w;i++) fputc('=',f); fputc('\n',f); }

/* ── rollup row for cross-rank aggregation ───────────────────────────────── */
typedef struct {
    char   name[MINIPROF_SYMBOL_LEN];
    long   total_calls;
    double total_incl_s;
    double total_excl_s;
    long   total_bytes;     /* only meaningful for MPI rows */
    long   max_calls;       /* for imbalance reporting       */
} roll_t;

static roll_t *g_rolls_for_sort = NULL;

static int cmp_roll_time_desc(const void *a, const void *b)
{
    int ia = *(const int*)a, ib = *(const int*)b;
    double ta = g_rolls_for_sort[ia].total_excl_s;
    double tb = g_rolls_for_sort[ib].total_excl_s;
    return (tb > ta) - (tb < ta);
}

void trace_aggregator_run(miniprof_trace_profile_t *local, const char *outfile)
{
    int world_rank, world_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    int msg_bytes = (int)sizeof(miniprof_trace_profile_t);
    miniprof_trace_profile_t *all = NULL;
    if (world_rank == 0)
        all = calloc(world_size, sizeof(miniprof_trace_profile_t));

    MPI_Gather(local, msg_bytes, MPI_BYTE, all, msg_bytes, MPI_BYTE,
              0, MPI_COMM_WORLD);

    if (world_rank != 0) { free(all); return; }

    /* ── roll up user functions across ranks ──────────────────────────────── */
    int max_rows = world_size * MINIPROF_TRACE_MAX_FUNCS + 1;
    roll_t *func_rolls = calloc(max_rows, sizeof(roll_t));
    int n_func_rolls = 0;

    for (int r = 0; r < world_size; r++) {
        for (int i = 0; i < all[r].n_funcs; i++) {
            miniprof_trace_func_t *fn = &all[r].funcs[i];
            int idx = -1;
            for (int k = 0; k < n_func_rolls; k++)
                if (strcmp(func_rolls[k].name, fn->name) == 0) { idx = k; break; }
            if (idx < 0) {
                idx = n_func_rolls++;
                snprintf(func_rolls[idx].name, MINIPROF_SYMBOL_LEN, "%s", fn->name);
            }
            func_rolls[idx].total_calls += fn->calls;
            func_rolls[idx].total_incl_s += fn->incl_time_s;
            func_rolls[idx].total_excl_s += fn->excl_time_s;
            if (fn->calls > func_rolls[idx].max_calls)
                func_rolls[idx].max_calls = fn->calls;
        }
    }

    /* ── roll up MPI calls across ranks ───────────────────────────────────── */
    roll_t *mpi_rolls = calloc(max_rows, sizeof(roll_t));
    int n_mpi_rolls = 0;

    for (int r = 0; r < world_size; r++) {
        for (int i = 0; i < all[r].n_mpi; i++) {
            miniprof_trace_mpi_t *m = &all[r].mpi[i];
            int idx = -1;
            for (int k = 0; k < n_mpi_rolls; k++)
                if (strcmp(mpi_rolls[k].name, m->name) == 0) { idx = k; break; }
            if (idx < 0) {
                idx = n_mpi_rolls++;
                snprintf(mpi_rolls[idx].name, MINIPROF_SYMBOL_LEN, "%s", m->name);
            }
            mpi_rolls[idx].total_calls += m->calls;
            mpi_rolls[idx].total_excl_s += m->total_time_s;
            mpi_rolls[idx].total_bytes += m->total_bytes;
        }
    }

    /* sort both by total exclusive/total time descending */
    int *forder = calloc(n_func_rolls + 1, sizeof(int));
    for (int i = 0; i < n_func_rolls; i++) forder[i] = i;
    g_rolls_for_sort = func_rolls;
    qsort(forder, n_func_rolls, sizeof(int), cmp_roll_time_desc);

    int *morder = calloc(n_mpi_rolls + 1, sizeof(int));
    for (int i = 0; i < n_mpi_rolls; i++) morder[i] = i;
    g_rolls_for_sort = mpi_rolls;
    qsort(morder, n_mpi_rolls, sizeof(int), cmp_roll_time_desc);

    /* ── write report ─────────────────────────────────────────────────────── */
    FILE *f = fopen(outfile, "w");
    if (!f) { perror("miniprof: fopen trace report"); goto done; }

    {
        time_t now = time(NULL);
        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

        const int W = 92;
        dline(f, W);
        fprintf(f, " miniprof — Tracing Profile (instrumented functions + MPI)\n");
        fprintf(f, " Generated : %s\n", tbuf);
        fprintf(f, " Ranks     : %d\n", world_size);
        fprintf(f, " Report    : %s\n", outfile);
        dline(f, W);
        fputc('\n', f);

        /* per-rank wall time */
        fprintf(f, " PER-RANK WALL TIME\n");
        hline(f, W);
        fprintf(f, "  %-6s  %14s\n", "Rank", "Wall time (s)");
        hline(f, W);
        for (int r = 0; r < world_size; r++)
            fprintf(f, "  %-6d  %14.3f\n", all[r].rank, all[r].wall_elapsed_s);
        fputc('\n', f);

        /* user function tracing table */
        fprintf(f, " Table 1: User Function Tracing "
                   "(aggregated across all ranks)\n");
        hline(f, W);
        fprintf(f, "  %8s  %12s  %12s  %10s  %10s  Function\n",
                "Calls", "Incl(s)", "Excl(s)", "Avg.Excl", "MaxCalls");
        hline(f, W);
        for (int oi = 0; oi < n_func_rolls; oi++) {
            roll_t *r = &func_rolls[forder[oi]];
            double avg_excl_us = r->total_calls
                                 ? (r->total_excl_s / r->total_calls) * 1e6 : 0.0;
            fprintf(f, "  %8ld  %12.4f  %12.4f  %8.2fus  %10ld  %s\n",
                    r->total_calls, r->total_incl_s, r->total_excl_s,
                    avg_excl_us, r->max_calls, r->name);
        }
        if (n_func_rolls == 0)
            fprintf(f, "  (no user functions matched the trace list — "
                       "check MINIPROF_TRACE_FUNCS / .mpa file)\n");
        fputc('\n', f);

        /* MPI tracing table */
        fprintf(f, " Table 2: MPI Call Tracing "
                   "(aggregated across all ranks)\n");
        hline(f, W);
        fprintf(f, "  %8s  %12s  %10s  %14s  %12s  Function\n",
                "Calls", "Time(s)", "Avg(us)", "Total Bytes", "Avg Bytes");
        hline(f, W);
        for (int oi = 0; oi < n_mpi_rolls; oi++) {
            roll_t *r = &mpi_rolls[morder[oi]];
            double avg_us = r->total_calls
                            ? (r->total_excl_s / r->total_calls) * 1e6 : 0.0;
            double avg_bytes = r->total_calls
                               ? (double)r->total_bytes / r->total_calls : 0.0;
            fprintf(f, "  %8ld  %12.4f  %8.2f  %14ld  %12.1f  %s\n",
                    r->total_calls, r->total_excl_s, avg_us,
                    r->total_bytes, avg_bytes, r->name);
        }
        if (n_mpi_rolls == 0)
            fprintf(f, "  (no MPI calls were wrapped in this build — "
                       "see mpi_trace.c for the wrapped subset)\n");
        fputc('\n', f);

        dline(f, W);
        fprintf(f, " miniprof tracing complete.  "
                   "User functions: %d   MPI functions: %d\n",
                n_func_rolls, n_mpi_rolls);
        dline(f, W);
        fclose(f);

        fprintf(stdout, "\n[miniprof] Trace report written to: %s\n\n", outfile);
    }

done:
    free(func_rolls);
    free(mpi_rolls);
    free(forder);
    free(morder);
    free(all);
}
