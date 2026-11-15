/* hotspots.h — opt-in function hotspots for snapshot mode
 *
 * MINIPROF_HOTSPOTS=1 (miniprof --hotspots) samples the program counter on
 * CPU time (ITIMER_PROF, every MINIPROF_HOTSPOTS_INTERVAL_MS, default 10 ms
 * of process CPU time) and, at finalize, names the leaf functions in-process
 * (no addr2line). Off by default: no timer, no signal handler, no cost.
 */
#ifndef MINIPROF_HOTSPOTS_H
#define MINIPROF_HOTSPOTS_H
#include "../include/miniprof.h"

#define HS_TOP 48                     /* functions each rank sends */

typedef struct {
    char name [MINIPROF_SYMBOL_LEN];
    char group[MINIPROF_GROUP_LEN];
    char dso  [48];
    long samples;
} hs_entry_t;

int  hotspots_requested(void);        /* MINIPROF_HOTSPOTS set and true  */
void hotspots_start(void);
void hotspots_stop(void);
int  hotspots_interval_ms(void);
/* this rank's functions, merged by name, most samples first; returns the
 * count (<= max) and the rank's total samples in *total */
int  hotspots_collect(hs_entry_t *out, int max, long *total);

#endif
