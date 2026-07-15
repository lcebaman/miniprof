#ifndef PATH_STORE_H
#define PATH_STORE_H

#include <stdint.h>

/*
 * path_store — lock-free store of whole call paths (PC sequences).
 *
 * Where profile_store counts individual PCs (flat profile), path_store
 * counts unique *sequences* of PCs, preserving caller→callee structure
 * so the aggregator can build a calltree ("Profile by Call Path").
 *
 * Async-signal-safe recording: FNV-1a hash of the PC array selects a
 * slot; the hash field is claimed with a CAS, then depth/pcs are
 * written and count incremented atomically. Collisions probe linearly.
 */

#define PATH_STORE_CAPACITY  16384         /* power of 2                    */
#define PATH_MAX_DEPTH       16            /* frames kept per path          */

typedef struct {
    uint64_t  hash;                        /* 0 = empty slot                */
    int32_t   depth;
    uintptr_t pcs[PATH_MAX_DEPTH];         /* leaf first (pcs[0] = leaf)    */
    long      count;
} path_entry_t;

void path_store_init(void);
void path_store_record(void **pcs, int n);
void path_store_iterate(int (*cb)(const path_entry_t *e, void *arg),
                        void *arg);

#endif
