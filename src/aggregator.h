#ifndef AGGREGATOR_H
#define AGGREGATOR_H

#include "../include/miniprof.h"

void aggregator_run(miniprof_rank_profile_t *local, const char *outfile,
                    const char *mpa_outfile, double mpa_threshold);

#endif
