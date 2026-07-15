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
    _Atomic(uint64_t)   cycles;         /* 0 if hardware counters disabled  */
    _Atomic(uint64_t)   instructions;
    _Atomic(uint64_t)   cache_misses;
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
 * Find or claim the slot for `pc`, via CAS on an empty slot with linear
 * probing on collision. Returns NULL if the table is full (sample is
 * still counted in g_total_samples, just dropped from the per-PC table).
 * Async-signal-safe — no malloc, no locks.
 */
static slot_t *claim_slot(uintptr_t pc)
{
    uint32_t idx = hash_pc(pc) & (STORE_CAPACITY - 1);

    for (int probe = 0; probe < STORE_CAPACITY; probe++) {
        uintptr_t slot_pc = atomic_load_explicit(&table[idx].pc,
                                                 memory_order_relaxed);
        if (slot_pc == pc) return &table[idx];

        if (slot_pc == 0) {
            /* try to claim this empty slot */
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &table[idx].pc, &expected, pc,
                    memory_order_relaxed, memory_order_relaxed)) {
                return &table[idx];
            }
            /* another thread won the CAS — check if they wrote the same PC */
            slot_pc = atomic_load_explicit(&table[idx].pc,
                                           memory_order_relaxed);
            if (slot_pc == pc) return &table[idx];
        }
        /* linear probe */
        idx = (idx + 1) & (STORE_CAPACITY - 1);
    }
    return NULL;
}

/*
 * Record one PC hit.  Called from SIGPROF handler — must be async-signal-safe.
 */
void profile_store_record(void *pc_ptr)
{
    uintptr_t pc = (uintptr_t)pc_ptr;
    if (!pc) return;

    atomic_fetch_add_explicit(&g_total_samples, 1L, memory_order_relaxed);

    slot_t *s = claim_slot(pc);
    if (s) atomic_fetch_add_explicit(&s->count, 1L, memory_order_relaxed);
}

/* Same as profile_store_record(), plus accumulating a hardware-counter
 * delta into the same slot. Same async-signal-safety constraints. */
void profile_store_record_hwc(void *pc_ptr, uint64_t cycles,
                              uint64_t instructions, uint64_t cache_misses)
{
    uintptr_t pc = (uintptr_t)pc_ptr;
    if (!pc) return;

    atomic_fetch_add_explicit(&g_total_samples, 1L, memory_order_relaxed);

    slot_t *s = claim_slot(pc);
    if (!s) return;
    atomic_fetch_add_explicit(&s->count, 1L, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->cycles, cycles, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->instructions, instructions,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s->cache_misses, cache_misses,
                              memory_order_relaxed);
}

long profile_store_total(void)
{
    return atomic_load(&g_total_samples);
}

/* Iterate over populated slots; cb returns non-zero to stop early */
void profile_store_iterate(int (*cb)(uintptr_t pc, long count,
                                     uint64_t cycles, uint64_t instructions,
                                     uint64_t cache_misses, void *arg),
                           void *arg)
{
    for (int i = 0; i < STORE_CAPACITY; i++) {
        uintptr_t pc    = atomic_load(&table[i].pc);
        long      count = atomic_load(&table[i].count);
        if (pc && count > 0) {
            uint64_t cycles = atomic_load(&table[i].cycles);
            uint64_t insns  = atomic_load(&table[i].instructions);
            uint64_t misses = atomic_load(&table[i].cache_misses);
            if (cb(pc, count, cycles, insns, misses, arg)) return;
        }
    }
}
