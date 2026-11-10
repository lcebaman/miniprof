/*
 * test_ompt_abi.c — src/ompt_abi.h (miniprof's own copy of the OMPT ABI,
 * so the library builds without an OpenMP toolchain) against the real
 * <omp-tools.h>. Compile-time only: it builds, or it doesn't.
 * `make test-ompt-abi` skips when no compiler ships omp-tools.h.
 */
#include <omp-tools.h>
#include <stddef.h>
#include "../src/ompt_abi.h"

#define SAME(a, b) _Static_assert((unsigned)(a) == (unsigned)(b), #a " != " #b)

SAME(MP_OMPT_CB_THREAD_BEGIN,     ompt_callback_thread_begin);
SAME(MP_OMPT_CB_THREAD_END,       ompt_callback_thread_end);
SAME(MP_OMPT_CB_PARALLEL_BEGIN,   ompt_callback_parallel_begin);
SAME(MP_OMPT_CB_PARALLEL_END,     ompt_callback_parallel_end);
SAME(MP_OMPT_CB_IMPLICIT_TASK,    ompt_callback_implicit_task);
SAME(MP_OMPT_CB_SYNC_REGION_WAIT, ompt_callback_sync_region_wait);
SAME(MP_OMPT_CB_MUTEX_ACQUIRE,    ompt_callback_mutex_acquire);
SAME(MP_OMPT_CB_MUTEX_ACQUIRED,   ompt_callback_mutex_acquired);
SAME(MP_OMPT_SET_NEVER,           ompt_set_never);
SAME(MP_OMPT_SET_ALWAYS,          ompt_set_always);
SAME(MP_OMPT_SCOPE_BEGIN,         ompt_scope_begin);
SAME(MP_OMPT_SCOPE_END,           ompt_scope_end);
SAME(MP_OMPT_SYNC_BARRIER,        ompt_sync_region_barrier);
SAME(MP_OMPT_SYNC_BARRIER_IMPLICIT,     ompt_sync_region_barrier_implicit);
SAME(MP_OMPT_SYNC_BARRIER_EXPLICIT,     ompt_sync_region_barrier_explicit);
SAME(MP_OMPT_SYNC_BARRIER_IMPLEMENTATION, ompt_sync_region_barrier_implementation);
SAME(MP_OMPT_SYNC_TASKWAIT,       ompt_sync_region_taskwait);
SAME(MP_OMPT_SYNC_TASKGROUP,      ompt_sync_region_taskgroup);
SAME(MP_OMPT_SYNC_REDUCTION,      ompt_sync_region_reduction);
SAME(MP_OMPT_SYNC_BARRIER_IMPLICIT_WORKSHARE, ompt_sync_region_barrier_implicit_workshare);
SAME(MP_OMPT_SYNC_BARRIER_IMPLICIT_PARALLEL,  ompt_sync_region_barrier_implicit_parallel);
SAME(MP_OMPT_SYNC_BARRIER_TEAMS,  ompt_sync_region_barrier_teams);
SAME(MP_OMPT_TASK_INITIAL,        ompt_task_initial);
SAME(MP_OMPT_TASK_IMPLICIT,       ompt_task_implicit);
SAME(MP_OMPT_THREAD_INITIAL,      ompt_thread_initial);
SAME(MP_OMPT_THREAD_WORKER,       ompt_thread_worker);
SAME(MP_OMPT_THREAD_OTHER,        ompt_thread_other);
SAME(MP_OMPT_PARALLEL_LEAGUE,     ompt_parallel_league);
SAME(MP_OMPT_PARALLEL_TEAM,       ompt_parallel_team);

_Static_assert(sizeof(mp_ompt_data_t) == sizeof(ompt_data_t), "ompt_data_t size");
_Static_assert(sizeof(mp_ompt_frame_t) == sizeof(ompt_frame_t), "ompt_frame_t size");
_Static_assert(sizeof(mp_ompt_start_tool_result_t) == sizeof(ompt_start_tool_result_t),
               "ompt_start_tool_result_t size");
_Static_assert(offsetof(mp_ompt_start_tool_result_t, tool_data) ==
               offsetof(ompt_start_tool_result_t, tool_data), "tool_data offset");

int main(void) { return 0; }
