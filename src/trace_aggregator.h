#ifndef TRACE_AGGREGATOR_H
#define TRACE_AGGREGATOR_H

#include "../include/miniprof.h"

void trace_aggregator_run(miniprof_trace_profile_t *local, const char *outfile,
                          double ipc_compute_threshold,
                          double ipc_memory_threshold,
                          double mpki_memory_threshold);

#endif
