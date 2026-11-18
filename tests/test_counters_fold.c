/* test_counters_fold.c — weighted events fold into one logical event.
 * Uses the swtest/fold group (software events): flops = 1 x task-clock
 * + 3 x task-clock, so flops must be 4 x instructions (= task-clock, to 0.1%:
 * the events of a group are read one after the other),
 * in the run totals and in a region, and the logical group must show 4
 * events (flops once), the physical one 5. */
#include "../src/counters.h"
#include "../include/miniprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "ok    " : "FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static volatile double g_x;
static void touch(int n) { for (long i = 0; i < n * 100000L; i++) g_x = g_x * 1.0000001 + 1e-9; }

static int near(uint64_t a, uint64_t b) { double d = (double)a - (double)b; return (d < 0 ? -d : d) <= 1e-3 * (double)b; }

int main(void)
{
    if (!cnt_init("swtest", "fold", 0)) {
        printf("SKIP: counters unavailable (software perf events blocked?)\n");
        return 0;
    }
    touch(400);
    miniprof_region_begin("r");
    touch(400);
    miniprof_region_end("r");
    cnt_region_t reg[4];
    int nr = cnt_regions_get(reg, 4);
    cnt_result_t r;
    cnt_collect(&r);
    const cnt_group_t *g = r.group;
    int iF = -1, iI = -1, nflops = 0;
    for (int e = 0; e < g->n_events; e++) {
        if (g->ev[e].kind == CK_FLOPS) { iF = e; nflops++; }
        if (g->ev[e].kind == CK_INSTRUCTIONS) iI = e;
    }
    CHECK(g->n_events == 4 && r.n_active == 4 && nflops == 1, "logical group: 4 events, flops once (n_events %d, active %d)",
          g->n_events, r.n_active);
    CHECK(g->n_required == 3, "logical required events: 3 (%d)", g->n_required);
    CHECK(iF >= 0 && iI >= 0 && r.total[iI] > 1000000 && near(r.total[iF], 4 * r.total[iI]),
          "run totals: flops %llu ~ 4 x %llu", (unsigned long long)r.total[iF], (unsigned long long)r.total[iI]);
    CHECK(nr == 1 && reg[0].ev[0][iI] > 1000000 && near(reg[0].ev[0][iF], 4 * reg[0].ev[0][iI]),
          "region: flops %llu ~ 4 x %llu", (unsigned long long)reg[0].ev[0][iF], (unsigned long long)reg[0].ev[0][iI]);
    printf("%s: counters fold (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
