#ifndef MINIPROF_SNAPSHOT_H
#define MINIPROF_SNAPSHOT_H

/*
 * snapshot.h — APS/uProfPcm-style whole-run characterisation mode
 *
 * MINIPROF_MODE=snapshot. Unlike sample/trace mode there is no per-sample
 * or per-call record: each rank keeps running totals, and at
 * MPI_Finalize they are reduced/gathered into one JSON document
 * (MINIPROF_SNAPSHOT_OUTPUT, default miniprof_snapshot.json) that every
 * later report — text verdict, HTML, `miniprof diff` — is generated from.
 *
 * MINIPROF_SNAPSHOT selects the ONE core-counter group a run collects
 * (a CPU has too few counters for all of them; see counters.c):
 *   basic    — cycles, instructions, FLOPs, DRAM fills (default)
 *   topdown  — the top-down event sets instead
 *   roofline — FLOPs + DRAM reads, for per-region roofline points
 *   full     — FP operations by vector width
 * Everything that is not a core counter (time split, MPI, I/O, OpenMP,
 * memory, hotspots, timeline) is collected at every level.
 *
 * docs/internals.md describes the whole snapshot pipeline.
 */

typedef enum {
    SNAPSHOT_BASIC = 0,
    SNAPSHOT_TOPDOWN,
    SNAPSHOT_ROOFLINE,
    SNAPSHOT_FULL
} snapshot_level_t;

#define MINIPROF_SNAPSHOT_SCHEMA         "miniprof-snapshot"
#define MINIPROF_SNAPSHOT_SCHEMA_VERSION "0.8.0"   /* 0.8: metrics.timeline */
#define MINIPROF_DEFAULT_SNAPSHOT_OUTPUT "miniprof_snapshot.json"

void snapshot_init(int verbose);
void snapshot_finalize(double elapsed_s);

/* Exposed for tests: parse a level name; returns -1 if unrecognised. */
int         snapshot_parse_level(const char *s);
const char *snapshot_level_name(snapshot_level_t lvl);

#endif /* MINIPROF_SNAPSHOT_H */
