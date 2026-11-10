#ifndef MINIPROF_OMPT_ABI_H
#define MINIPROF_OMPT_ABI_H

/*
 * ompt_abi.h — the slice of the OpenMP tools interface (OMPT, OpenMP 5.x)
 * that miniprof uses, declared locally.
 *
 * Why not #include <omp-tools.h>: it ships with LLVM-based compilers
 * (clang, AOCC, ICX) but not with GCC, and miniprof must build with any
 * MPI wrapper compiler. The values below are fixed by the OpenMP
 * specification (5.0 onwards, unchanged in 5.1/5.2), so declaring them is
 * safe; tests/test_ompt_abi.c checks every constant and struct layout
 * against the real header wherever one is available (`make test-ompt-abi`).
 *
 * Everything is prefixed mp_ so it can never collide with the real header
 * if an application translation unit includes both.
 */

#include <stdint.h>

typedef union mp_ompt_data_t {
    uint64_t value;
    void    *ptr;
} mp_ompt_data_t;

typedef struct mp_ompt_frame_t {
    mp_ompt_data_t exit_frame;
    mp_ompt_data_t enter_frame;
    int exit_frame_flags;
    int enter_frame_flags;
} mp_ompt_frame_t;

/* ompt_callbacks_t (subset) */
enum {
    MP_OMPT_CB_THREAD_BEGIN     = 1,
    MP_OMPT_CB_THREAD_END       = 2,
    MP_OMPT_CB_PARALLEL_BEGIN   = 3,
    MP_OMPT_CB_PARALLEL_END     = 4,
    MP_OMPT_CB_IMPLICIT_TASK    = 7,
    MP_OMPT_CB_SYNC_REGION_WAIT = 16,
    MP_OMPT_CB_MUTEX_ACQUIRE    = 26,
    MP_OMPT_CB_MUTEX_ACQUIRED   = 27,
};

/* ompt_set_result_t */
enum {
    MP_OMPT_SET_ERROR            = 0,
    MP_OMPT_SET_NEVER            = 1,
    MP_OMPT_SET_IMPOSSIBLE       = 2,
    MP_OMPT_SET_SOMETIMES        = 3,
    MP_OMPT_SET_SOMETIMES_PAIRED = 4,
    MP_OMPT_SET_ALWAYS           = 5,
};

/* ompt_scope_endpoint_t */
enum {
    MP_OMPT_SCOPE_BEGIN    = 1,
    MP_OMPT_SCOPE_END      = 2,
    MP_OMPT_SCOPE_BEGINEND = 3,
};

/* ompt_sync_region_t */
enum {
    MP_OMPT_SYNC_BARRIER                    = 1,  /* deprecated in 5.1 */
    MP_OMPT_SYNC_BARRIER_IMPLICIT           = 2,  /* deprecated in 5.1 */
    MP_OMPT_SYNC_BARRIER_EXPLICIT           = 3,
    MP_OMPT_SYNC_BARRIER_IMPLEMENTATION     = 4,
    MP_OMPT_SYNC_TASKWAIT                   = 5,
    MP_OMPT_SYNC_TASKGROUP                  = 6,
    MP_OMPT_SYNC_REDUCTION                  = 7,
    MP_OMPT_SYNC_BARRIER_IMPLICIT_WORKSHARE = 8,
    MP_OMPT_SYNC_BARRIER_IMPLICIT_PARALLEL  = 9,
    MP_OMPT_SYNC_BARRIER_TEAMS              = 10,
};

/* ompt_task_flag_t (subset) */
enum {
    MP_OMPT_TASK_INITIAL  = 0x00000001,
    MP_OMPT_TASK_IMPLICIT = 0x00000002,
};

/* ompt_thread_t */
enum {
    MP_OMPT_THREAD_INITIAL = 1,
    MP_OMPT_THREAD_WORKER  = 2,
    MP_OMPT_THREAD_OTHER   = 3,
};

/* ompt_parallel_flag_t (subset) */
enum {
    MP_OMPT_PARALLEL_LEAGUE = 0x40000000,
    MP_OMPT_PARALLEL_TEAM   = (int)0x80000000,
};

typedef void (*mp_ompt_interface_fn_t)(void);
typedef mp_ompt_interface_fn_t (*mp_ompt_function_lookup_t)(const char *name);
typedef void (*mp_ompt_callback_t)(void);
typedef int  (*mp_ompt_set_callback_t)(int event, mp_ompt_callback_t callback);

typedef int  (*mp_ompt_initialize_t)(mp_ompt_function_lookup_t lookup,
                                     int initial_device_num,
                                     mp_ompt_data_t *tool_data);
typedef void (*mp_ompt_finalize_t)(mp_ompt_data_t *tool_data);

typedef struct mp_ompt_start_tool_result_t {
    mp_ompt_initialize_t initialize;
    mp_ompt_finalize_t   finalize;
    mp_ompt_data_t       tool_data;
} mp_ompt_start_tool_result_t;

#endif
