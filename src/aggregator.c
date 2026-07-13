/*
 * aggregator.c — MPI_Gather flat profiles and write a text report
 *
 * Report layout:
 *
 *   Table 1: Profile by Group, Function, and Line
 *   Samp% | Samp | Imb.Samp | Imb.Samp% | Group
 *                                       | Function
 *                                       | Source Line
 *
 * Imbalance definition:
 *   Imb.Samp  = max_rank_samples - mean_rank_samples
 *   Imb.Samp% = Imb.Samp / max_rank_samples * 100
 *
 * We send each rank's full (function,file,line) table as raw bytes
 * (MPI_BYTE) to avoid struct-alignment / MPI-type-construction pitfalls.
 *
 * NOTE: all qsort comparators are plain file-scope statics (no nested
 * functions / GCC trampolines) so the resulting binary does not require
 * an executable stack.
 */

#include "aggregator.h"
#include "mpa_writer.h"
#include "../include/miniprof.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── per-function rollup row ─────────────────────────────────────────────── */
#define MAX_RANKS_TRACKED 1024

typedef struct {
    char group[MINIPROF_GROUP_LEN];
    char name [MINIPROF_SYMBOL_LEN];
    char dso  [MINIPROF_SYMBOL_LEN];
    long total;
    long per_rank[MAX_RANKS_TRACKED];
} func_roll_t;

typedef struct { char group[MINIPROF_GROUP_LEN]; long total; } grp_roll_t;

/* ── module-scope state used only during one aggregator_run() call ──────── *
 * aggregator_run is never re-entrant / never called concurrently (single
 * rank-0 process, single call per program run), so this is safe and avoids
 * nested-function trampolines that would force an executable stack.        */
static long   *g_e_total;
static func_roll_t *g_funcs;
static long   *g_rank_col;     /* scratch: per_rank[] column for current rank during sort */

/* ── separator helpers ───────────────────────────────────────────────────── */
static void hline(FILE *f, int w) { for (int i = 0; i < w; i++) fputc('-', f); fputc('\n', f); }
static void dline(FILE *f, int w) { for (int i = 0; i < w; i++) fputc('=', f); fputc('\n', f); }

/* ── comparators (file-scope, no closures) ───────────────────────────────── */
static int cmp_entry_total_desc(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    long ta = g_e_total[ia], tb = g_e_total[ib];
    return (tb > ta) - (tb < ta);
}

static int cmp_func_total_desc(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    long ta = g_funcs[ia].total, tb = g_funcs[ib].total;
    return (tb > ta) - (tb < ta);
}

static int cmp_grp_total_desc(const void *a, const void *b)
{
    const grp_roll_t *ga = a, *gb = b;
    return (gb->total > ga->total) - (gb->total < ga->total);
}

/* compares funcs[] indices by g_rank_col[] (caller fills g_rank_col first) */
static int cmp_func_rankcol_desc(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    long ca = g_rank_col[ia], cb = g_rank_col[ib];
    return (cb > ca) - (cb < ca);
}

/* ── main entry point ────────────────────────────────────────────────────── */
void aggregator_run(miniprof_rank_profile_t *local, const char *outfile,
                    const char *mpa_outfile, double mpa_threshold)
{
    int world_rank, world_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    int msg_bytes = (int)sizeof(miniprof_rank_profile_t);

    miniprof_rank_profile_t *all = NULL;
    if (world_rank == 0)
        all = calloc(world_size, sizeof(miniprof_rank_profile_t));

    MPI_Gather(local, msg_bytes, MPI_BYTE,
               all,   msg_bytes, MPI_BYTE,
               0, MPI_COMM_WORLD);

    if (world_rank != 0) { free(all); return; }

    int ranks_tracked = world_size < MAX_RANKS_TRACKED
                         ? world_size : MAX_RANKS_TRACKED;

    /* ── build global (group,function,file,line) table ───────────────────── */
    int max_entries = world_size * MINIPROF_MAX_SEND_SYMS + 1;

    char  (*e_group)[MINIPROF_GROUP_LEN]  = calloc(max_entries, MINIPROF_GROUP_LEN);
    char  (*e_name )[MINIPROF_SYMBOL_LEN] = calloc(max_entries, MINIPROF_SYMBOL_LEN);
    char  (*e_dso  )[MINIPROF_SYMBOL_LEN] = calloc(max_entries, MINIPROF_SYMBOL_LEN);
    char  (*e_file )[MINIPROF_SYMBOL_LEN] = calloc(max_entries, MINIPROF_SYMBOL_LEN);
    int    *e_line   = calloc(max_entries, sizeof(int));
    long   *e_total  = calloc(max_entries, sizeof(long));
    long   *e_perrank= calloc((size_t)max_entries * world_size, sizeof(long));
    int     n_entries = 0;

    for (int r = 0; r < world_size; r++) {
        miniprof_rank_profile_t *rp = &all[r];
        for (int s = 0; s < rp->n_syms; s++) {
            miniprof_sym_t *sym = &rp->syms[s];
            if (!sym->name[0]) continue;

            int idx = -1;
            for (int e = 0; e < n_entries; e++) {
                if (strcmp(e_name[e], sym->name) == 0 &&
                    strcmp(e_file[e], sym->file) == 0 &&
                    e_line[e] == sym->line) {
                    idx = e; break;
                }
            }
            if (idx < 0) {
                if (n_entries >= max_entries) continue;
                idx = n_entries++;
                snprintf(e_group[idx], MINIPROF_GROUP_LEN, "%s", sym->group);
                snprintf(e_name [idx], MINIPROF_SYMBOL_LEN, "%s", sym->name);
                snprintf(e_dso  [idx], MINIPROF_SYMBOL_LEN, "%s", sym->dso);
                snprintf(e_file [idx], MINIPROF_SYMBOL_LEN, "%s", sym->file);
                e_line[idx] = sym->line;
            }
            e_total[idx] += sym->count;
            e_perrank[(size_t)idx * world_size + r] += sym->count;
        }
    }

    long grand_samples = 0;
    for (int r = 0; r < world_size; r++) grand_samples += all[r].total_samples;

    /* sort entries by total_count descending */
    g_e_total = e_total;
    int *order = calloc(n_entries, sizeof(int));
    for (int i = 0; i < n_entries; i++) order[i] = i;
    qsort(order, n_entries, sizeof(int), cmp_entry_total_desc);

    /* ── roll up by FUNCTION (across all its lines) ───────────────────────── */
    int max_funcs = n_entries + 1;
    func_roll_t *funcs = calloc(max_funcs, sizeof(func_roll_t));
    int n_funcs = 0;

    for (int oi = 0; oi < n_entries; oi++) {
        int e = order[oi];
        int fidx = -1;
        for (int fi = 0; fi < n_funcs; fi++) {
            if (strcmp(funcs[fi].name, e_name[e]) == 0 &&
                strcmp(funcs[fi].dso,  e_dso[e])  == 0) { fidx = fi; break; }
        }
        if (fidx < 0) {
            fidx = n_funcs++;
            snprintf(funcs[fidx].group, MINIPROF_GROUP_LEN, "%s", e_group[e]);
            snprintf(funcs[fidx].name,  MINIPROF_SYMBOL_LEN, "%s", e_name[e]);
            snprintf(funcs[fidx].dso,   MINIPROF_SYMBOL_LEN, "%s", e_dso[e]);
        }
        funcs[fidx].total += e_total[e];
        for (int r = 0; r < ranks_tracked; r++)
            funcs[fidx].per_rank[r] += e_perrank[(size_t)e * world_size + r];
    }

    /* sort functions by total descending */
    g_funcs = funcs;
    int *forder = calloc(n_funcs, sizeof(int));
    for (int i = 0; i < n_funcs; i++) forder[i] = i;
    qsort(forder, n_funcs, sizeof(int), cmp_func_total_desc);

    /* ── group subtotals ──────────────────────────────────────────────────── */
    grp_roll_t grps[8]; int n_grps = 0;
    const char *known_groups[] = {
        MINIPROF_GROUP_USER, MINIPROF_GROUP_MPI, MINIPROF_GROUP_OMP,
        MINIPROF_GROUP_LIBC, MINIPROF_GROUP_SYS, MINIPROF_GROUP_PROFILER,
        MINIPROF_GROUP_OTHER
    };
    for (int g = 0; g < 7; g++) {
        long tot = 0;
        for (int fi = 0; fi < n_funcs; fi++)
            if (strcmp(funcs[fi].group, known_groups[g]) == 0) tot += funcs[fi].total;
        if (tot > 0) {
            snprintf(grps[n_grps].group, MINIPROF_GROUP_LEN, "%s", known_groups[g]);
            grps[n_grps].total = tot;
            n_grps++;
        }
    }
    qsort(grps, n_grps, sizeof(grp_roll_t), cmp_grp_total_desc);

    /* ── write MPA suggestions file ─────────────────────────────────────── */
    if (mpa_outfile && mpa_outfile[0]) {
        const char **names  = calloc(n_funcs, sizeof(char *));
        double       *pcts  = calloc(n_funcs, sizeof(double));
        const char  **grps2 = calloc(n_funcs, sizeof(char *));
        for (int oi = 0; oi < n_funcs; oi++) {
            int fi = forder[oi];
            names [oi] = funcs[fi].name;
            grps2 [oi] = funcs[fi].group;
            pcts  [oi] = grand_samples
                         ? 100.0 * funcs[fi].total / grand_samples : 0.0;
        }
        mpa_write_suggestions(mpa_outfile, names, pcts, grps2, n_funcs,
                              mpa_threshold);
        free(names); free((void*)pcts); free(grps2);
    }

    /* ── write report ──────────────────────────────────────────────────────── */
    FILE *f = fopen(outfile, "w");
    if (!f) { perror("miniprof: fopen report"); goto done; }

    {
        time_t now = time(NULL);
        char   tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

        const int W = 88;
        dline(f, W);
        fprintf(f, " miniprof — Parallel Sampling Profile\n");
        fprintf(f, " Generated : %s\n", tbuf);
        fprintf(f, " Ranks     : %d\n", world_size);
        fprintf(f, " Report    : %s\n", outfile);
        dline(f, W);
        fputc('\n', f);

        /* per-rank summary */
        fprintf(f, " PER-RANK SUMMARY\n");
        hline(f, W);
        fprintf(f, "  %-6s  %10s  %14s\n", "Rank", "Samples", "Wall time (s)");
        hline(f, W);
        for (int r = 0; r < world_size; r++)
            fprintf(f, "  %-6d  %10ld  %14.3f\n",
                    all[r].rank, all[r].total_samples, all[r].wall_elapsed_s);
        hline(f, W);
        fprintf(f, "  %-6s  %10ld\n\n", "TOTAL", grand_samples);

        /* Table 1: Profile by Group, Function, and Line */
        fprintf(f, " Table 1: Profile by Group, Function, and Line\n");
        hline(f, W);
        fprintf(f, "  %6s  %8s  %9s  %9s  Group\n",
                "Samp%", "Samp", "Imb.Samp", "Imb.Samp%");
        fprintf(f, "  %6s  %8s  %9s  %9s   Function\n", "", "", "", "");
        fprintf(f, "  %6s  %8s  %9s  %9s    Source Line\n", "", "", "", "");
        hline(f, W);

        double total_pct = grand_samples ? 100.0 : 0.0;
        fprintf(f, "  %5.1f%%  %8ld  %9s  %9s  %s\n",
                total_pct, grand_samples, "--", "--", "Total");

        /* scratch buffer for sorting line rows under a function */
        int *line_idx = calloc(n_entries + 1, sizeof(int));

        for (int g = 0; g < n_grps; g++) {
            double gpct = grand_samples ? 100.0 * grps[g].total / grand_samples : 0.0;
            fprintf(f, "  %5.1f%%  %8ld  %9s  %9s  %s\n",
                    gpct, grps[g].total, "--", "--", grps[g].group);

            for (int oi = 0; oi < n_funcs; oi++) {
                int fi = forder[oi];
                if (strcmp(funcs[fi].group, grps[g].group) != 0) continue;

                double fpct = grand_samples
                              ? 100.0 * funcs[fi].total / grand_samples : 0.0;

                /* imbalance: max_rank - mean, as a percentage of max */
                long max_r = 0, sum_r = 0;
                for (int r = 0; r < ranks_tracked; r++) {
                    if (funcs[fi].per_rank[r] > max_r) max_r = funcs[fi].per_rank[r];
                    sum_r += funcs[fi].per_rank[r];
                }
                double mean_r   = ranks_tracked ? (double)sum_r / ranks_tracked : 0.0;
                double imb_samp = max_r - mean_r;
                double imb_pct  = max_r > 0 ? 100.0 * imb_samp / max_r : 0.0;

                fprintf(f, "  %5.1f%%  %8ld  %9.1f  %8.1f%%   %s\n",
                        fpct, funcs[fi].total, imb_samp, imb_pct, funcs[fi].name);

                /* line rows under this function, sorted by count desc,
                   capped at MINIPROF_MAX_LINES_PER_FUNC */
                int n_lines = 0;
                for (int e = 0; e < n_entries; e++) {
                    if (strcmp(e_name[e], funcs[fi].name) == 0 &&
                        strcmp(e_dso[e],  funcs[fi].dso)  == 0 &&
                        e_line[e] > 0) {
                        line_idx[n_lines++] = e;
                    }
                }
                qsort(line_idx, n_lines, sizeof(int), cmp_entry_total_desc);

                int show_lines = n_lines < MINIPROF_MAX_LINES_PER_FUNC
                                 ? n_lines : MINIPROF_MAX_LINES_PER_FUNC;
                for (int li = 0; li < show_lines; li++) {
                    int e = line_idx[li];
                    double lpct = grand_samples
                                  ? 100.0 * e_total[e] / grand_samples : 0.0;
                    fprintf(f, "  %5.1f%%  %8ld  %9s  %9s    %s:%d\n",
                            lpct, e_total[e], "--", "--",
                            e_file[e][0] ? e_file[e] : "?", e_line[e]);
                }
            }
        }
        free(line_idx);
        fputc('\n', f);

        /* per-rank breakdown */
        fprintf(f, " PER-RANK BREAKDOWN (top %d functions per rank)\n",
                MINIPROF_TOP_PER_RANK);
        hline(f, W);

        int   *ridx     = calloc(n_funcs + 1, sizeof(int));
        long  *rank_col = calloc(n_funcs + 1, sizeof(long));
        g_rank_col = rank_col;

        for (int r = 0; r < world_size; r++) {
            long rtotal = all[r].total_samples > 0 ? all[r].total_samples : 1;
            fprintf(f, "  Rank %3d : ", r);

            int n_r = 0;
            for (int fi = 0; fi < n_funcs; fi++) {
                long c = (r < ranks_tracked) ? funcs[fi].per_rank[r] : 0;
                if (c > 0) {
                    ridx[n_r]     = fi;
                    rank_col[fi]  = c;   /* index by fi so comparator can read it */
                    n_r++;
                }
            }
            qsort(ridx, n_r, sizeof(int), cmp_func_rankcol_desc);

            int top = n_r < MINIPROF_TOP_PER_RANK ? n_r : MINIPROF_TOP_PER_RANK;
            if (top == 0) { fprintf(f, "(no samples)\n"); continue; }
            for (int i = 0; i < top; i++) {
                long c = rank_col[ridx[i]];
                double pct = 100.0 * c / rtotal;
                fprintf(f, "%.24s %.1f%%", funcs[ridx[i]].name, pct);
                if (i < top-1) fprintf(f, "  |  ");
            }
            fputc('\n', f);
        }
        free(ridx);
        free(rank_col);
        fputc('\n', f);

        dline(f, W);
        fprintf(f, " miniprof complete.  Functions: %d   "
                   "(func,line) entries: %d\n", n_funcs, n_entries);
        dline(f, W);
        fclose(f);

        fprintf(stdout, "\n[miniprof] Report written to: %s\n\n", outfile);
    }

done:
    free(e_group); free(e_name); free(e_dso); free(e_file);
    free(e_line); free(e_total); free(e_perrank);
    free(order); free(funcs); free(forder);
    free(all);
}
