#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>

/* Must be a power of 2 and >= MINIPROF_MAX_PCS */
#define STORE_CAPACITY  65536

void  profile_store_init(void);
void  profile_store_record(void *pc);

/* Same as profile_store_record(), plus accumulating a hardware-counter
 * delta (cycles/instructions/cache-misses) into the same PC's slot. */
void  profile_store_record_hwc(void *pc, uint64_t cycles,
                               uint64_t instructions, uint64_t cache_misses);

long  profile_store_total(void);
void  profile_store_iterate(int (*cb)(uintptr_t pc, long count,
                                      uint64_t cycles, uint64_t instructions,
                                      uint64_t cache_misses, void *arg),
                            void *arg);

#endif
