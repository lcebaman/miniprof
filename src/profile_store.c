/*
 * profile_store.c — lock-free PC→count hash table
 *
 * The signal handler calls profile_store_record() which only does:
 *   1. open-addressing hash lookup on the PC
 *   2. atomic increment of the hit counter
 *
 * No malloc, no locks, no stdio — safe inside SIGPROF handler.
 * Symbolisation happens later at finalize time.
 */

#include "profile_store.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

/* ── internal slot ───────────────────────────────────────────────────────── */
typedef struct {
    _Atomic(uintptr_t)  pc;      /* 0 = empty                               */
    _Atomic(long)       count;
} slot_t;

static slot_t table[STORE_CAPACITY];
static _Atomic(long) g_total_samples;

/* FNV-1a on a pointer value */
static inline uint32_t hash_pc(uintptr_t pc)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < (int)sizeof(uintptr_t); i++) {
        h ^= (uint8_t)(pc & 0xff);
        h *= 16777619u;
        pc >>= 8;
    }
    return h;
}

void profile_store_init(void)
{
    memset(table, 0, sizeof(table));
    atomic_store(&g_total_samples, 0L);
}

/*
 * Record one PC hit.  Called from SIGPROF handler — must be async-signal-safe.
 * Uses CAS to claim an empty slot; falls through silently if table is full.
 */
void profile_store_record(void *pc_ptr)
{
    uintptr_t pc = (uintptr_t)pc_ptr;
    if (!pc) return;

    uint32_t idx = hash_pc(pc) & (STORE_CAPACITY - 1);
    atomic_fetch_add_explicit(&g_total_samples, 1L, memory_order_relaxed);

    for (int probe = 0; probe < STORE_CAPACITY; probe++) {
        uintptr_t slot_pc = atomic_load_explicit(&table[idx].pc,
                                                 memory_order_relaxed);
        if (slot_pc == pc) {
            /* our PC is already in this slot — just bump the count */
            atomic_fetch_add_explicit(&table[idx].count, 1L,
                                      memory_order_relaxed);
            return;
        }
        if (slot_pc == 0) {
            /* try to claim this empty slot */
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &table[idx].pc, &expected, pc,
                    memory_order_relaxed, memory_order_relaxed)) {
                atomic_fetch_add_explicit(&table[idx].count, 1L,
                                          memory_order_relaxed);
                return;
            }
            /* another thread won the CAS — check if they wrote the same PC */
            slot_pc = atomic_load_explicit(&table[idx].pc,
                                           memory_order_relaxed);
            if (slot_pc == pc) {
                atomic_fetch_add_explicit(&table[idx].count, 1L,
                                          memory_order_relaxed);
                return;
            }
        }
        /* linear probe */
        idx = (idx + 1) & (STORE_CAPACITY - 1);
    }
    /* table full — sample dropped (counted in total but not per-PC) */
}

long profile_store_total(void)
{
    return atomic_load(&g_total_samples);
}

/* Iterate over populated slots; cb returns non-zero to stop early */
void profile_store_iterate(int (*cb)(uintptr_t pc, long count, void *arg),
                           void *arg)
{
    for (int i = 0; i < STORE_CAPACITY; i++) {
        uintptr_t pc    = atomic_load(&table[i].pc);
        long      count = atomic_load(&table[i].count);
        if (pc && count > 0) {
            if (cb(pc, count, arg)) return;
        }
    }
}
