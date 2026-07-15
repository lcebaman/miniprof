/*
 * path_store.c — lock-free hash store of call paths for the sampler
 *
 * Same design philosophy as profile_store.c (CAS-claimed open-addressed
 * hash, safe to call from the SIGPROF handler) but keyed on the whole
 * PC sequence instead of a single PC.
 *
 * Slot claim protocol:
 *   1. Compute FNV-1a hash h of the PC array (h forced nonzero).
 *   2. Probe linearly from h & MASK.
 *   3. Empty slot (hash==0): CAS(hash, 0, h). Winner writes depth+pcs,
 *      then increments count. A concurrent reader could in principle see
 *      hash set but pcs half-written; in this profiler recording happens
 *      only inside the SIGPROF handler of a single-threaded process, and
 *      iteration happens only after sampler_stop(), so there is no
 *      concurrent reader in practice — the CAS is defensive.
 *   4. Occupied slot with matching hash: verify depth+pcs match (hash
 *      collisions across different paths are possible); on match,
 *      atomic-increment count; on mismatch, keep probing.
 *   5. Table full after CAPACITY probes: drop the sample (counted in
 *      g_dropped for the curious; never blocks, never allocates).
 */

#include "path_store.h"
#include <string.h>

static path_entry_t g_paths[PATH_STORE_CAPACITY];
static long         g_dropped = 0;

#define MASK (PATH_STORE_CAPACITY - 1)

void path_store_init(void)
{
    memset(g_paths, 0, sizeof(g_paths));
    g_dropped = 0;
}

static inline uint64_t fnv1a(void **pcs, int n)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        uintptr_t v = (uintptr_t)pcs[i];
        for (int b = 0; b < (int)sizeof(v); b++) {
            h ^= (v >> (b * 8)) & 0xff;
            h *= 1099511628211ULL;
        }
    }
    return h ? h : 1;   /* 0 is the empty-slot marker */
}

void path_store_record(void **pcs, int n)
{
    if (n <= 0) return;
    if (n > PATH_MAX_DEPTH) n = PATH_MAX_DEPTH;

    uint64_t h = fnv1a(pcs, n);
    uint64_t idx = h & MASK;

    for (int probe = 0; probe < PATH_STORE_CAPACITY; probe++) {
        path_entry_t *e = &g_paths[idx];
        uint64_t cur = __atomic_load_n(&e->hash, __ATOMIC_ACQUIRE);

        if (cur == 0) {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(&e->hash, &expected, h, 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                e->depth = n;
                for (int i = 0; i < n; i++)
                    e->pcs[i] = (uintptr_t)pcs[i];
                __atomic_fetch_add(&e->count, 1, __ATOMIC_RELAXED);
                return;
            }
            cur = __atomic_load_n(&e->hash, __ATOMIC_ACQUIRE);
        }

        if (cur == h && e->depth == n) {
            int match = 1;
            for (int i = 0; i < n; i++)
                if (e->pcs[i] != (uintptr_t)pcs[i]) { match = 0; break; }
            if (match) {
                __atomic_fetch_add(&e->count, 1, __ATOMIC_RELAXED);
                return;
            }
        }
        idx = (idx + 1) & MASK;
    }
    __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
}

void path_store_iterate(int (*cb)(const path_entry_t *e, void *arg),
                        void *arg)
{
    for (int i = 0; i < PATH_STORE_CAPACITY; i++) {
        if (g_paths[i].hash != 0 && g_paths[i].count > 0)
            if (!cb(&g_paths[i], arg))
                return;
    }
}
