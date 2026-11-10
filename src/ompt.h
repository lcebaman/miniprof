#ifndef MINIPROF_OMPT_H
#define MINIPROF_OMPT_H

/*
 * ompt.h — OpenMP measurement for snapshot mode via the OMPT tools
 * interface (phase P2).
 *
 * The OpenMP runtime discovers miniprof through the ompt_start_tool()
 * symbol exported by libminiprof.so. Runtimes that implement OMPT: LLVM
 * libomp and its derivatives (clang, AOCC, Intel ICX/IFX, Cray CCE).
 * GCC's libgomp does NOT implement OMPT, so GCC-built OpenMP codes report
 * "openmp": {"available": false} with the reason.
 *
 * The tool only activates when MINIPROF_MODE=snapshot (it returns NULL to
 * the runtime otherwise, so other modes pay nothing) and MINIPROF_OMPT is
 * not "0". Statistics are reset at snapshot_init (MPI_Init) and frozen at
 * finalize, so they cover the same window as every other metric.
 */

#include <stdint.h>

#define OMP_MAX_REGIONS 256
#define OMP_MAX_THREADS 1024

typedef struct {
    const void *codeptr;        /* return address of the parallel construct */
    uint64_t calls;             /* times the region was entered             */
    uint64_t wall_ns;           /* encountering thread, begin → end         */
    uint64_t task_ns;           /* sum over team members of implicit tasks  */
    uint64_t wait_ns;           /* sum over team members of barrier waits   */
    uint64_t tasks;             /* implicit tasks (= sum of team sizes)     */
    uint64_t max_team;
} omp_region_t;

typedef struct {
    int      available;          /* runtime started the tool              */
    char     runtime[96];        /* runtime_version passed by the runtime */
    char     reason[160];        /* when !available                       */
    int      threads;            /* OpenMP threads seen                   */
    uint64_t outer_parallel_ns;  /* initial thread inside outermost regions */
    uint64_t work_ns, wait_ns, lock_ns, taskwait_ns;   /* summed over threads */
    uint64_t work_min_ns, work_max_ns;                 /* over threads        */
    int      nregions;
    omp_region_t regions[OMP_MAX_REGIONS];
} omp_result_t;

void ompt_snapshot_begin(void);   /* reset + start recording (MPI_Init)  */
void ompt_snapshot_end(void);     /* stop recording (finalize)           */
void ompt_collect(omp_result_t *out);

#endif
