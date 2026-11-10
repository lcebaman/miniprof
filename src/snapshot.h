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
 * MINIPROF_SNAPSHOT selects which counter groups a run collects, so the
 * groups never have to be multiplexed onto the PMU together:
 *   basic    — time split, CPU utilisation, base counters (default)
 *   topdown  — basic + top-down pipeline utilisation group
 *   roofline — basic + FLOP/byte groups for the roofline
 *   full     — everything (multiplexed; for quick looks, not for numbers)
 *
 * Phase P0 (this file's first version) implements the scaffolding only:
 * mode switch, level selection, node/rank metadata, overhead accounting
 * and the versioned JSON container. Every metric block is present in the
 * schema as `null` until the phase that fills it lands.
 */

typedef enum {
    SNAPSHOT_BASIC = 0,
    SNAPSHOT_TOPDOWN,
    SNAPSHOT_ROOFLINE,
    SNAPSHOT_FULL
} snapshot_level_t;

#define MINIPROF_SNAPSHOT_SCHEMA         "miniprof-snapshot"
#define MINIPROF_SNAPSHOT_SCHEMA_VERSION "0.4.0"
#define MINIPROF_DEFAULT_SNAPSHOT_OUTPUT "miniprof_snapshot.json"

void snapshot_init(int verbose);
void snapshot_finalize(double elapsed_s);

/* Exposed for tests: parse a level name; returns -1 if unrecognised. */
int         snapshot_parse_level(const char *s);
const char *snapshot_level_name(snapshot_level_t lvl);

#endif /* MINIPROF_SNAPSHOT_H */
