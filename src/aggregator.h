#ifndef AGGREGATOR_H
#define AGGREGATOR_H

#include "../include/miniprof.h"

void aggregator_run(miniprof_rank_profile_t *local, const char *outfile,
                    const char *mpa_outfile, double mpa_threshold,
                    double ipc_compute_threshold, double ipc_memory_threshold,
                    double llc_memory_threshold, int interval_ms);

#endif
