#ifndef SAMPLER_H
#define SAMPLER_H

#include "../include/miniprof.h"   /* for MINIPROF_MAX_DEPTH */

void sampler_start(int interval_ms, int depth);
void sampler_stop(void);

#endif
