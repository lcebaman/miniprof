#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>

/* Must be a power of 2 and >= MINIPROF_MAX_PCS */
#define STORE_CAPACITY  65536

void  profile_store_init(void);
void  profile_store_record(void *pc);
long  profile_store_total(void);
void  profile_store_iterate(int (*cb)(uintptr_t pc, long count, void *arg),
                            void *arg);

#endif
