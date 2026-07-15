#ifndef TRACE_STORE_H
#define TRACE_STORE_H

#include <stdint.h>

void trace_store_init(void);
void trace_store_enter(void *pc);
void trace_store_exit(void *pc);

/* cycles/instructions/cache_misses are INCLUSIVE per call (entry-to-exit
 * delta, not adjusted for time spent in children the way excl_s is) —
 * 0 for all three if hardware counters were unavailable for this run. */
void trace_store_iterate(void (*cb)(uintptr_t pc, long calls,
                                    double incl_s, double excl_s,
                                    uint64_t cycles, uint64_t instructions,
                                    uint64_t cache_misses, void *arg),
                         void *arg);

/* Bridge used by mpi_trace.c so a traced user function's exclusive time
   correctly excludes time spent inside MPI calls it made. */
double trace_store_mpi_time_total(void);
void   trace_store_add_mpi_time(double seconds);

#endif
