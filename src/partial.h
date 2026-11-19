#ifndef PARTIAL_H
#define PARTIAL_H
/*
 * partial.h — a per-rank record when a run ends without MPI_Finalize.
 *
 * A job stopped by its time limit (SIGTERM from the scheduler), interrupted
 * (SIGINT), ended by MPI_Abort, or calling exit() without MPI_Finalize
 * writes no snapshot: the snapshot needs every rank in collectives. So each
 * rank writes what it has on its own, <snapshot>.partial/rank.<r>.json, and
 * tools/miniprof_partial.py merges whatever ranks wrote into a snapshot
 * marked partial. Nothing is written by a run that finalizes normally.
 *
 * partial_dump runs in a signal handler: it reads only data kept in atomics
 * or fixed at init, formats integers by hand, and uses open/write/rename.
 */
#include <time.h>

typedef struct {
    int rank, nranks;
    const char *output;            /* the snapshot path: records go next to it */
    const char *level, *start_utc, *command;
    const char *hostname, *uarch, *uarch_name, *brand;
    int cpus_allowed, cores_allowed;
} partial_info_t;

void partial_prepare(const partial_info_t *info);   /* MPI_Init: copy what the record needs */
void partial_arm(const struct timespec *app_start); /* handlers, atexit; the app's clock    */
void partial_disarm(void);                          /* MPI_Finalize: a full snapshot follows */
void partial_dump(const char *reason, int signo);   /* async-signal-safe; once per process  */

#endif
