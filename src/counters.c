/*
 * counters.c — snapshot-mode hardware counter engine. See counters.h.
 *
 * ── Event tables ────────────────────────────────────────────────────────
 * Encodings are from the Linux kernel's perf event definitions
 * (tools/perf/pmu-events/arch/x86/amdzen4 and amdzen5), which follow the
 * AMD PPRs. Zen 4 and Zen 5 differ where it matters: fp_ret_sse_avx_ops.all
 * is umask 0x1f on Zen 4 but 0x0f on Zen 5 (Zen 5 reuses the high umask
 * bits for a precision field), so each generation has its own table.
 *
 * ── Why groups have an optional tail ────────────────────────────────────
 * Zen exposes 6 general-purpose core counters per thread and no fixed
 * ones; fp_ret_sse_avx_ops (FLOPs) is a large-increment event that takes
 * two of them. When the NMI watchdog is on it permanently occupies one
 * more, so a group that needs all 6 passes perf_event_open's validation
 * but is never scheduled (time_running stays 0, every count reads 0), and
 * one that needs 7 fails to open. cnt_init() tries the full group, then
 * trims optional events from the end — on open failure or if the group
 * never ran — until it fits or only the required events are left.
 *
 * ── rdpmc fast path ─────────────────────────────────────────────────────
 * Each event fd's first page is mmapped; when the kernel sets
 * cap_user_rdpmc and the event is on the PMU (index != 0) the count is
 * offset + sign-extended rdpmc(index - 1), read under the page's seqlock
 * (the documented protocol in linux/perf_event.h). If any event of the
 * group is not currently scheduled, the whole group falls back to one
 * read() — never mixing the two within one snapshot.
 */

#define _GNU_SOURCE
#include "counters.h"
#include "../include/miniprof.h"

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── tables ──────────────────────────────────────────────────────────── */

#define RAW(ev, um) PERF_TYPE_RAW, CNT_AMD_RAW(ev, um)
/* ... with a counter mask: count cycles where the event fires >= cm times */
#define RAWC(ev, um, cm) PERF_TYPE_RAW, (CNT_AMD_RAW(ev, um) | ((uint64_t)(cm) << 24))
#define HW(cfg)     PERF_TYPE_HARDWARE, PERF_COUNT_HW_##cfg

/* Older entries leave config1, weight and no_rdpmc out: zero. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const cnt_group_t k_groups[] = {
    /* base: cycles, instructions, FLOPs required. fp_ret_sse_avx_ops is a
     * large-increment event that occupies TWO counters, so the required
     * part already uses 4 of the 6; the optional tail is ordered by value
     * and trimmed from the end until the group fits (5 counters with the
     * NMI watchdog on, 6 without). Measured on EPYC 9754 / RHEL 9.7:
     * a 6-counter group never schedules while nmi_watchdog=1. */
    { "zen4", "base", 3, 6, {
        { CK_CYCLES,            "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_INSTRUCTIONS,      "ex_ret_instr",                       RAW(0x0c0, 0x00) },
        { CK_FLOPS,             "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x1f) },
        { CK_DRAM_FILLS,        "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_FILLS_REMOTE, "ls_any_fills_from_sys.dram_io_far",  RAW(0x044, 0x40) },
        { CK_L2_MISSES,         "l2_cache_req_stat.ic_dc_miss_in_l2", RAW(0x064, 0x09) },
    } },
    { "zen5", "base", 3, 6, {
        { CK_CYCLES,            "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_INSTRUCTIONS,      "ex_ret_instr",                       RAW(0x0c0, 0x00) },
        { CK_FLOPS,             "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x0f) },
        { CK_DRAM_FILLS,        "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_FILLS_REMOTE, "ls_any_fills_from_sys.dram_io_far",  RAW(0x044, 0x40) },
        { CK_L2_MISSES,         "l2_cache_req_stat.ic_dc_miss_in_l2", RAW(0x064, 0x09) },
    } },
    /* roofline: DRAM reads by region. A streaming read is mostly brought in
     * by the L2 prefetcher; the L1D fill then comes from L2 and the fill
     * event does not see it. Fills from DRAM + L2-prefetcher requests that
     * miss L2 and L3 (event 0x72, umask 0x1f: L2 prefetchers only, so the
     * L1-prefetcher requests already counted as fills are not doubled)
     * covered 99.9% of the known reads of five kernels on EPYC 9754
     * (tools/probe_fills.sh: dot, sum, daxpy, copy, strided); fills alone
     * 0.1-15%. 5 counters: fits with the NMI watchdog on. */
    { "zen4", "roofline", 4, 4, {
        { CK_CYCLES,     "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_FLOPS,      "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x1f) },
        { CK_DRAM_FILLS, "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_L2PF,  "l2_pf_miss_l2_l3.l2_hwpf",           RAW(0x072, 0x1f) },
    } },
    { "zen5", "roofline", 4, 4, {
        { CK_CYCLES,     "ls_not_halted_cyc",                  RAW(0x076, 0x00) },
        { CK_FLOPS,      "fp_ret_sse_avx_ops.all",             RAW(0x003, 0x0f) },
        { CK_DRAM_FILLS, "ls_any_fills_from_sys.dram_io_all",  RAW(0x044, 0x48) },
        { CK_DRAM_L2PF,  "l2_pf_miss_l2_l3.l2_hwpf",           RAW(0x072, 0x1f) },
    } },
    /* Zen 5 FLOP precision split, for the roofline group (P6). The precision
     * field (umask bits 7:5) only filters: it must be combined with the
     * op-type bits 3:0. Measured on EPYC 9555 (perf 5.14 RHEL 9.7), the
     * four variants below sum exactly to .all (0x0f); the kernel JSON's
     * 0x40/0x60/0x80/0xa0 count nothing:
     *   0x4f scalar single   0x6f packed single
     *   0x8f scalar double   0xaf packed double
     * Each is a two-counter event, so at most two fit in one group with
     * the NMI watchdog on. Zen 4 has no precision field. */

    /* vector: FP ops by width (one counter each) + cycles = 5 counters,
     * fits with the watchdog on. FLOPs (2 counters) would not fit with
     * all four widths; they come from the base group. */
    { "zen4", "vector", 4, 5, {
        { CK_FP_SCALAR, "fp_ops_retired_by_width.scalar_uops_retired",   RAW(0x008, 0x04) },
        { CK_FP_128,    "fp_ops_retired_by_width.pack_128_uops_retired", RAW(0x008, 0x08) },
        { CK_FP_256,    "fp_ops_retired_by_width.pack_256_uops_retired", RAW(0x008, 0x10) },
        { CK_FP_512,    "fp_ops_retired_by_width.pack_512_uops_retired", RAW(0x008, 0x20) },
        { CK_CYCLES,    "ls_not_halted_cyc",                          RAW(0x076, 0x00) },
    } },
    { "zen5", "vector", 4, 5, {
        { CK_FP_SCALAR, "fp_ops_retired_by_width.scalar_uops_retired",   RAW(0x008, 0x04) },
        { CK_FP_128,    "fp_ops_retired_by_width.pack_128_uops_retired", RAW(0x008, 0x08) },
        { CK_FP_256,    "fp_ops_retired_by_width.pack_256_uops_retired", RAW(0x008, 0x10) },
        { CK_FP_512,    "fp_ops_retired_by_width.pack_512_uops_retired", RAW(0x008, 0x20) },
        { CK_CYCLES,    "ls_not_halted_cyc",                          RAW(0x076, 0x00) },
    } },
    /* ── top-down (P4) ─────────────────────────────────────────────────
     * AMD's pipeline-utilisation model (perf metric groups PipelineL1/L2;
     * the kernel's amdzen4/amdzen5 JSON), in dispatch slots: W per cycle,
     * W = 6 on Zen 4 and 8 on Zen 5.
     *   L1  frontend  = fe_empty_slots / slots
     *       bad spec  = (dispatched ops - retired ops) / slots
     *       backend   = be_stall_slots / slots
     *       retiring  = retired ops / slots      smt = smt_slots / slots
     *   L2  frontend latency  share = W * fe_lat_cycles / fe_empty_slots
     *       backend memory    share = noret_load / noret_notcomplete
     *       microcode         share = ucode_ops / retired ops
     *       mispredicts       share = br_misp / (br_misp + resyncs)
     * Twelve events do not fit in 5 counters (6, one taken by the NMI
     * watchdog), so they are split into three sets of at most 5, each
     * carrying what its ratios need: no ratio mixes two sets. Each thread
     * counts one set for the whole run (see cnt_init). L2 shares from
     * td_febe/td_rbs are applied to the L1 fractions from td_l1.
 * Encodings checked against perf on EPYC 9754 (tools/validate_topdown.sh);
 * ex_ret_ucode_ops is event 0x1c2. Zen 5 has no resyncs_or_nc_redirects
 * (perf rejects the name on EPYC 9555); its PipelineL2 splits bad
 * speculation with bp_redirects.resync instead. */
    { "zen4", "td_l1", 5, 6, {
        { CK_CYCLES,         "ls_not_halted_cyc",                            RAW(0x076, 0x00) },
        { CK_RET_OPS,        "ex_ret_ops",                                   RAW(0x0c1, 0x00) },
        { CK_DISP_OPS,       "de_src_op_disp.all",                           RAW(0x0aa, 0x07) },
        { CK_FE_EMPTY_SLOTS, "de_no_dispatch_per_slot.no_ops_from_frontend", RAW(0x1a0, 0x01) },
        { CK_BE_STALL_SLOTS, "de_no_dispatch_per_slot.backend_stalls",       RAW(0x1a0, 0x1e) },
        { CK_SMT_SLOTS,      "de_no_dispatch_per_slot.smt_contention",       RAW(0x1a0, 0x60) },
    } },
    { "zen4", "td_febe", 5, 5, {
        { CK_CYCLES,            "ls_not_halted_cyc",                            RAW(0x076, 0x00) },
        { CK_FE_EMPTY_SLOTS,    "de_no_dispatch_per_slot.no_ops_from_frontend", RAW(0x1a0, 0x01) },
        { CK_FE_LAT_CYCLES,     "de_no_dispatch_per_slot.no_ops_from_frontend,cmask=0x6", RAWC(0x1a0, 0x01, 6) },
        { CK_NORET_LOAD,        "ex_no_retire.load_not_complete",               RAW(0x0d6, 0xa2) },
        { CK_NORET_NOTCOMPLETE, "ex_no_retire.not_complete",                    RAW(0x0d6, 0x02) },
    } },
    { "zen4", "td_rbs", 5, 5, {
        { CK_CYCLES,   "ls_not_halted_cyc",       RAW(0x076, 0x00) },
        { CK_RET_OPS,  "ex_ret_ops",              RAW(0x0c1, 0x00) },
        { CK_UCODE_OPS,"ex_ret_ucode_ops",        RAW(0x1c2, 0x00) },
        { CK_BR_MISP,  "ex_ret_brn_misp",         RAW(0x0c3, 0x00) },
        { CK_RESYNCS,  "resyncs_or_nc_redirects", RAW(0x096, 0x00) },
    } },
    { "zen5", "td_l1", 5, 6, {
        { CK_CYCLES,         "ls_not_halted_cyc",                            RAW(0x076, 0x00) },
        { CK_RET_OPS,        "ex_ret_ops",                                   RAW(0x0c1, 0x00) },
        { CK_DISP_OPS,       "de_src_op_disp.all",                           RAW(0x0aa, 0x07) },
        { CK_FE_EMPTY_SLOTS, "de_no_dispatch_per_slot.no_ops_from_frontend", RAW(0x1a0, 0x01) },
        { CK_BE_STALL_SLOTS, "de_no_dispatch_per_slot.backend_stalls",       RAW(0x1a0, 0x1e) },
        { CK_SMT_SLOTS,      "de_no_dispatch_per_slot.smt_contention",       RAW(0x1a0, 0x60) },
    } },
    { "zen5", "td_febe", 5, 5, {
        { CK_CYCLES,            "ls_not_halted_cyc",                            RAW(0x076, 0x00) },
        { CK_FE_EMPTY_SLOTS,    "de_no_dispatch_per_slot.no_ops_from_frontend", RAW(0x1a0, 0x01) },
        { CK_FE_LAT_CYCLES,     "de_no_dispatch_per_slot.no_ops_from_frontend,cmask=0x8", RAWC(0x1a0, 0x01, 8) },
        { CK_NORET_LOAD,        "ex_no_retire.load_not_complete",               RAW(0x0d6, 0xa2) },
        { CK_NORET_NOTCOMPLETE, "ex_no_retire.not_complete",                    RAW(0x0d6, 0x02) },
    } },
    { "zen5", "td_rbs", 5, 5, {
        { CK_CYCLES,   "ls_not_halted_cyc",       RAW(0x076, 0x00) },
        { CK_RET_OPS,  "ex_ret_ops",              RAW(0x0c1, 0x00) },
        { CK_UCODE_OPS,"ex_ret_ucode_ops",        RAW(0x1c2, 0x00) },
        { CK_BR_MISP,  "ex_ret_brn_misp",         RAW(0x0c3, 0x00) },
        { CK_RESYNCS,  "bp_redirects.resync",     RAW(0x09f, 0x01) },
    } },
    /* Test-only top-down sets: software events, to exercise the set
     * rotation, per-set results and regions without a PMU. The metrics
     * they produce are meaningless. */
    { "swtest", "td_l1", 5, 5, {
        { CK_CYCLES,         "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_RET_OPS,        "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_DISP_OPS,       "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_FE_EMPTY_SLOTS, "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
        { CK_BE_STALL_SLOTS, "cpu-migrations",   PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS },
    } },
    { "swtest", "td_febe", 5, 5, {
        { CK_CYCLES,            "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_FE_EMPTY_SLOTS,    "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_FE_LAT_CYCLES,     "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
        { CK_NORET_LOAD,        "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_NORET_NOTCOMPLETE, "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
    } },
    { "swtest", "td_rbs", 5, 5, {
        { CK_CYCLES,    "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_RET_OPS,   "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_UCODE_OPS, "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
        { CK_BR_MISP,   "cpu-migrations",   PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS },
        { CK_RESYNCS,   "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
    } },
    /* Test-only: weighted parts folded into one logical event
     * (tests/test_counters_fold.c): flops = 1 x task-clock + 3 x
     * task-clock, instructions = task-clock (the only software event that
     * counts with exclude_kernel). */
    { "swtest", "fold", 4, 5, {
        { CK_CYCLES,       "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, 0, 0 },
        { CK_FLOPS,        "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 1 },
        { CK_INSTRUCTIONS, "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_FLOPS,        "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 3 },
        { CK_L2_MISSES,    "cpu-migrations",   PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS, 0, 0 },
    } },
    /* Test-only (MINIPROF_COUNTERS_TEST=tma): the Intel top-down path
     * (one td_metrics set, read() only) with software events. */
    { "swtest_tma", "td_metrics", 9, 10, {
        { CK_SLOTS,        "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_TD_RETIRING,  "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_TD_BADSPEC,   "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, 0, 0 },
        { CK_TD_FE,        "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_TD_BE,        "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_TD_HEAVY,     "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, 0, 0 },
        { CK_TD_BRMISP,    "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, 0, 0 },
        { CK_TD_FETCHLAT,  "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_TD_MEMBOUND,  "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, 0 },
        { CK_UOP_DROPPING, "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, 0, 0 },
    }, 1 },
    /* Test-only (MINIPROF_COUNTERS_TEST=1): software events, so the whole
     * engine — groups, thread slots, pthread hook, MPI boundaries,
     * regions, collect — can be exercised on machines without a PMU
     * (CI, VMs). task-clock (ns on CPU) stands in for cycles. */
    { "swtest", "base", 3, 5, {
        { CK_CYCLES,       "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_INSTRUCTIONS, "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_FLOPS,        "minor-faults",     PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN },
        { CK_L2_MISSES,    "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
        { CK_DRAM_FILLS,   "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
    } },
    { "swtest", "roofline", 4, 4, {
        { CK_CYCLES,     "task-clock",       PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { CK_FLOPS,      "minor-faults",     PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN },
        { CK_DRAM_FILLS, "page-faults",      PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
        { CK_DRAM_L2PF,  "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES },
    } },
    /* ── Intel Granite Rapids (Redwood Cove cores) ─────────────────────
     * Encodings from the kernel's graniterapids JSON (tools/perf/pmu-events/
     * arch/x86/graniterapids), checked with tools/probe_intel.sh.
     * 8 general-purpose counters per thread plus fixed counters for
     * instructions, cycles and top-down slots; cycles and instructions use
     * the fixed counters unless the NMI watchdog holds the cycles one.
     *
     * FLOPs: FP_ARITH_INST_RETIRED counts instructions by width and
     * precision (an FMA counts twice), so FLOPs = scalar + 2 x 128-bit
     * double + 4 x (128-bit single + 256-bit double) + 8 x (256-bit single
     * + 512-bit double) + 16 x 512-bit single: five events, weighted and
     * folded into one logical "flops" (perf's FLOPs metric does the same).
     * FP16 (FP_ARITH_INST_RETIRED2) and AMX are not counted.
     *
     * DRAM: OCR.READS_TO_CORE.DRAM counts every cacheable read, RFO and
     * code request served by DRAM, demand and prefetch (L1 and L2
     * prefetchers), local or remote socket: the core's whole DRAM read
     * traffic in lines. Offcore-response events need an MSR (config1) and
     * run on counters 0-3; there are two MSRs, used by event 0x2a and 0x2b. */
#define IR(ev, um)        PERF_TYPE_RAW, CNT_INTEL_RAW(ev, um), 0
#define IOCR(ev, rsp)     PERF_TYPE_RAW, CNT_INTEL_RAW(ev, 0x01), (rsp)
#define FP(um, w, name)   { CK_FLOPS, name, IR(0xc7, um), w }
    { "granite_rapids", "base", 7, 10, {
        { CK_CYCLES,       "cycles",       HW(CPU_CYCLES), 0, 0 },
        { CK_INSTRUCTIONS, "instructions", HW(INSTRUCTIONS), 0, 0 },
        FP(0x03,  1, "fp_arith_inst_retired.scalar + 2x .128b_packed_double + 4x .4_flops + 8x .8_flops + 16x .512b_packed_single"),
        FP(0x04,  2, "fp_arith_inst_retired.128b_packed_double"),
        FP(0x18,  4, "fp_arith_inst_retired.4_flops"),
        FP(0x60,  8, "fp_arith_inst_retired.8_flops"),
        FP(0x80, 16, "fp_arith_inst_retired.512b_packed_single"),
        { CK_DRAM_READS,        "ocr.reads_to_core.dram",        IOCR(0x2a, 0x73C004477ull), 0 },
        { CK_DRAM_READS_REMOTE, "ocr.reads_to_core.remote_dram", IOCR(0x2b, 0x730004477ull), 0 },
        { CK_L2_MISSES,         "l2_rqsts.miss",                 IR(0x24, 0x3f), 0 },
    } },
    { "granite_rapids", "roofline", 8, 8, {
        { CK_CYCLES,       "cycles",       HW(CPU_CYCLES), 0, 0 },
        { CK_INSTRUCTIONS, "instructions", HW(INSTRUCTIONS), 0, 0 },
        FP(0x03,  1, "fp_arith_inst_retired.scalar + 2x .128b_packed_double + 4x .4_flops + 8x .8_flops + 16x .512b_packed_single"),
        FP(0x04,  2, "fp_arith_inst_retired.128b_packed_double"),
        FP(0x18,  4, "fp_arith_inst_retired.4_flops"),
        FP(0x60,  8, "fp_arith_inst_retired.8_flops"),
        FP(0x80, 16, "fp_arith_inst_retired.512b_packed_single"),
        { CK_DRAM_READS, "ocr.reads_to_core.dram", IOCR(0x2a, 0x73C004477ull), 0 },
    } },
    /* vector: arithmetic FP instructions by width (both precisions). Unlike
     * Zen's FP-unit ops these leave out loads, stores and moves, and an FMA
     * counts twice. */
    { "granite_rapids", "vector", 4, 6, {
        { CK_FP_SCALAR, "fp_arith_inst_retired.scalar",                     IR(0xc7, 0x03), 0 },
        { CK_FP_128,    "fp_arith_inst_retired.128b_packed_double+single",  IR(0xc7, 0x0c), 0 },
        { CK_FP_256,    "fp_arith_inst_retired.256b_packed_double+single",  IR(0xc7, 0x30), 0 },
        { CK_FP_512,    "fp_arith_inst_retired.512b_packed_double+single",  IR(0xc7, 0xc0), 0 },
        { CK_CYCLES,       "cycles",       HW(CPU_CYCLES), 0, 0 },
        { CK_INSTRUCTIONS, "instructions", HW(INSTRUCTIONS), 0, 0 },
    } },
    /* top-down: Intel TMA through perf's metric events. The slots fixed
     * counter leads the group; the topdown-* events are read from the
     * PERF_METRICS register (no general-purpose counter) and come back
     * as slot counts. Levels 1 and 2 in one group, so no set rotation.
     * Metric events cannot be read with rdpmc as counts: read() only.
     * Encodings: the kernel's cpu/events/slots (event=0,umask=4) and
     * topdown-* (event=0, umask=0x80..0x87). */
    { "granite_rapids", "td_metrics", 9, 11, {
        { CK_SLOTS,        "slots",                 IR(0x00, 0x04), 0 },
        { CK_TD_RETIRING,  "topdown-retiring",      IR(0x00, 0x80), 0 },
        { CK_TD_BADSPEC,   "topdown-bad-spec",      IR(0x00, 0x81), 0 },
        { CK_TD_FE,        "topdown-fe-bound",      IR(0x00, 0x82), 0 },
        { CK_TD_BE,        "topdown-be-bound",      IR(0x00, 0x83), 0 },
        { CK_TD_HEAVY,     "topdown-heavy-ops",     IR(0x00, 0x84), 0 },
        { CK_TD_BRMISP,    "topdown-br-mispredict", IR(0x00, 0x85), 0 },
        { CK_TD_FETCHLAT,  "topdown-fetch-lat",     IR(0x00, 0x86), 0 },
        { CK_TD_MEMBOUND,  "topdown-mem-bound",     IR(0x00, 0x87), 0 },
        { CK_UOP_DROPPING, "int_misc.uop_dropping", IR(0xad, 0x10), 0 },
        { CK_CYCLES,       "cycles",                HW(CPU_CYCLES), 0, 0 },
    }, 1 },
#undef FP
#undef IOCR
#undef IR
    /* anything else: architectural events only */
    { "generic", "base", 2, 2, {
        { CK_CYCLES,       "cycles",       HW(CPU_CYCLES) },
        { CK_INSTRUCTIONS, "instructions", HW(INSTRUCTIONS) },
    } },
};
#pragma GCC diagnostic pop
#define N_GROUPS (int)(sizeof(k_groups) / sizeof(k_groups[0]))

static const char *k_kind_names[CK_COUNT] = {
    "cycles", "instructions", "flops", "l2_misses", "dram_fills",
    "dram_fills_remote", "fp_scalar_ops", "fp_packed128_ops",
    "fp_packed256_ops", "fp_packed512_ops",
    "ret_ops", "disp_ops", "fe_empty_slots", "be_stall_slots", "smt_slots",
    "fe_latency_cycles", "noret_load_cycles", "noret_notcomplete_cycles",
    "ucode_ops", "br_misp", "resyncs", "dram_l2pf",
    "dram_reads", "dram_reads_remote", "slots", "td_retiring", "td_bad_spec",
    "td_fe_bound", "td_be_bound", "td_heavy_ops", "td_br_mispredict",
    "td_fetch_lat", "td_mem_bound", "uop_dropping"
};

int cnt_dispatch_width(const char *uarch)
{
    if (!uarch) return 0;
    if (strcmp(uarch, "zen4") == 0) return 6;
    if (strcmp(uarch, "zen5") == 0) return 8;
    if (strcmp(uarch, "zen6") == 0) return 8;
    if (strcmp(uarch, "granite_rapids") == 0) return 6;   /* slots come from the counter */
    if (strcmp(uarch, "swtest") == 0 || strcmp(uarch, "swtest_tma") == 0) return 1;
    return 0;
}

/* Generations that use another's tables. Zen 6 (Venice): on EPYC 9996
 * (family 1Ah model 51h, perf 5.14 RHEL 9.7) perf applies the Zen 5 JSON
 * and every core event used here has the Zen 5 encoding; FLOPs (.all,
 * umask 0x0f), fills, L2 misses, the by-width split and the PipelineL1/L2
 * events all count, and PipelineL1 sums to ~100% with W = 8
 * (tools/pmu_probe.sh). The counters block reports the table used. */
static const char *table_uarch(const char *uarch)
{
    if (uarch && strcmp(uarch, "zen6") == 0) return "zen5";
    return uarch;
}

/* exact (uarch, group) match, NULL if absent */
static const cnt_group_t *find_group(const char *uarch, const char *group)
{
    uarch = table_uarch(uarch);
    for (int i = 0; i < N_GROUPS; i++)
        if (uarch && strcmp(k_groups[i].uarch, uarch) == 0 &&
            strcmp(k_groups[i].group, group) == 0)
            return &k_groups[i];
    return NULL;
}

_Static_assert(sizeof(k_kind_names) / sizeof(k_kind_names[0]) == CK_COUNT, "one name per kind");

const char *cnt_kind_name(cnt_kind_t k)
{
    return (k >= 0 && k < CK_COUNT) ? k_kind_names[k] : "?";
}

const cnt_group_t *cnt_lookup(const char *uarch, const char *group)
{
    const cnt_group_t *g = find_group(uarch, group);
    if (g) return g;
    for (int i = 0; i < N_GROUPS; i++)
        if (strcmp(k_groups[i].uarch, "generic") == 0) return &k_groups[i];
    return &k_groups[0];   /* unreachable: generic is in the table */
}

/* ── per-thread state ────────────────────────────────────────────────── */

enum { CB_COMPUTE = 0, CB_MPI = 1, CB_IDLE = 2, CB_N = 3 };   /* count buckets */

#define CNT_MAX_THREADS 1024

typedef struct {
    int      region;          /* index into g_regions */
    uint64_t t0_ns;
    uint64_t v0[CNT_MAX_EVENTS];
} region_frame_t;

typedef struct {
    int      fd[CNT_MAX_EVENTS];
    struct perf_event_mmap_page *pg[CNT_MAX_EVENTS];
    int      bucket;                   /* where counts go now: CB_*        */
    int      omp_worker;               /* has run an OpenMP implicit task  */
    uint64_t last[CNT_MAX_EVENTS];
    uint64_t acc[CB_N][CNT_MAX_EVENTS];
    int      depth;
    region_frame_t stack[CNT_REGION_DEPTH];
    uint64_t n_rdpmc, n_syscall;       /* which read path boundaries took */
    pid_t    tid;                      /* kernel thread id the group counts */
    int      set;                      /* event set this thread counts      */
} thread_slot_t;

static thread_slot_t g_slots[CNT_MAX_THREADS];
static int  g_nslots = 0;                     /* atomic */
static int  g_dropped = 0;                    /* atomic */
static _Thread_local thread_slot_t *t_slot = NULL;
static _Thread_local int t_open_failed = 0;

/* Event sets: one for basic/roofline, CNT_MAX_SETS for top-down. */
static const cnt_group_t *g_sets[CNT_MAX_SETS];
static int  g_set_nev[CNT_MAX_SETS];   /* events in use per set          */
static int  g_nsets = 1;
static int  g_rank = 0;
#define NEV(s)  g_set_nev[(s)->set]
#define GRP(s)  g_sets[(s)->set]
static int  g_active = 0;        /* counting enabled                      */
static int  g_regions_on = 0;    /* regions record time even w/o counters */
static int  g_use_rdpmc = 0;
static int  g_split = 1;         /* reads at MPI calls (compute/MPI split) */
static char g_reason[160] = "";
static long g_pagesize = 4096;
static thread_slot_t *g_main_slot = NULL;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── perf plumbing ───────────────────────────────────────────────────── */

static long sys_perf_event_open(struct perf_event_attr *a, pid_t pid, int group_fd)
{
    return syscall(__NR_perf_event_open, a, pid /* 0 = this thread */, -1, group_fd, 0);
}

static inline pid_t my_tid(void) { return (pid_t)syscall(SYS_gettid); }

static void close_slot(thread_slot_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s->pg[i]) munmap(s->pg[i], (size_t)g_pagesize);
        if (s->fd[i] >= 0) close(s->fd[i]);
        s->pg[i] = NULL;
        s->fd[i] = -1;
    }
}

/* Open `n` events of the group for the calling thread, disabled; leader
 * first. Returns 0 or -errno. */
static int g_fail_index = -1;   /* index of the event that failed to open */

static int open_events_pid(thread_slot_t *s, int n, pid_t pid)
{
    for (int i = 0; i < CNT_MAX_EVENTS; i++) { s->fd[i] = -1; s->pg[i] = NULL; }
    for (int i = 0; i < n; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof(a));
        a.size           = sizeof(a);
        a.type           = GRP(s)->ev[i].type;
        a.config         = GRP(s)->ev[i].config;
        a.config1        = GRP(s)->ev[i].config1;
        a.disabled       = (i == 0);       /* members follow the leader */
        a.exclude_kernel = 1;
        a.exclude_hv     = 1;
        a.read_format    = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
        int fd = (int)sys_perf_event_open(&a, pid, i == 0 ? -1 : s->fd[0]);
        if (fd < 0) { int e = errno; close_slot(s, i); g_fail_index = i; return -e; }
        s->fd[i] = fd;
        if (g_use_rdpmc && !GRP(s)->no_rdpmc) {
            void *p = mmap(NULL, (size_t)g_pagesize, PROT_READ, MAP_SHARED, fd, 0);
            s->pg[i] = (p == MAP_FAILED) ? NULL : p;
        }
    }
    if (ioctl(s->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
        ioctl(s->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        int e = errno; close_slot(s, n); return -e;
    }
    return 0;
}

static int open_events(thread_slot_t *s, int n) { return open_events_pid(s, n, 0); }

typedef struct {
    uint64_t nr, time_enabled, time_running;
    uint64_t v[CNT_MAX_EVENTS];
} group_read_t;

static int read_group(const thread_slot_t *s, group_read_t *g)
{
    ssize_t want = (ssize_t)((3 + NEV(s)) * sizeof(uint64_t));
    return read(s->fd[0], g, sizeof(*g)) >= want ? 0 : -1;
}

#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t rdpmc(uint32_t c)
{
    uint32_t lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(c));
    return ((uint64_t)hi << 32) | lo;
}
#define HAVE_RDPMC 1
#else
#define HAVE_RDPMC 0
#endif

/* One event via its mmap page; 0 on success, -1 if not on the PMU. */
static inline int rdpmc_one(struct perf_event_mmap_page *pc, uint64_t *out)
{
#if HAVE_RDPMC
    uint32_t seq, idx;
    uint64_t count;
    do {
        seq = pc->lock;
        __asm__ volatile("" ::: "memory");
        idx = pc->index;
        count = (uint64_t)pc->offset;
        if (!pc->cap_user_rdpmc || idx == 0) return -1;
        uint64_t pmc = rdpmc(idx - 1);
        int w = pc->pmc_width;
        pmc <<= 64 - w;
        pmc = (uint64_t)((int64_t)pmc >> (64 - w));   /* sign-extend */
        count += pmc;
        __asm__ volatile("" ::: "memory");
    } while (pc->lock != seq);
    *out = count;
    return 0;
#else
    (void)pc; (void)out;
    return -1;
#endif
}

/* Current counts of the calling thread's group. */
static int read_now(thread_slot_t *s, uint64_t *v)
{
    if (g_use_rdpmc && !GRP(s)->no_rdpmc) {
        int ok = 1;
        for (int i = 0; i < NEV(s) && ok; i++)
            ok = s->pg[i] && rdpmc_one(s->pg[i], &v[i]) == 0;
        if (ok) { s->n_rdpmc++; return 0; }
    }
    group_read_t g;
    if (read_group(s, &g) != 0) return -1;
    s->n_syscall++;
    for (int i = 0; i < NEV(s); i++) v[i] = g.v[i];
    return 0;
}

/* ── thread slots ────────────────────────────────────────────────────── */

/* slot already opened for this kernel thread (attached at init)? */
static thread_slot_t *slot_by_tid(pid_t tid)
{
    int n = __atomic_load_n(&g_nslots, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_THREADS) n = CNT_MAX_THREADS;
    for (int i = 0; i < n; i++)
        if (g_slots[i].tid == tid && g_slots[i].fd[0] >= 0) return &g_slots[i];
    return NULL;
}

static thread_slot_t *thread_open(void)
{
    if (t_slot) return t_slot;
    if (!g_active || t_open_failed) return NULL;
    pid_t tid = my_tid();
    thread_slot_t *found = slot_by_tid(tid);
    if (found) { t_slot = found; return found; }
    int idx = __atomic_fetch_add(&g_nslots, 1, __ATOMIC_ACQ_REL);
    if (idx >= CNT_MAX_THREADS) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        t_open_failed = 1;
        return NULL;
    }
    thread_slot_t *s = &g_slots[idx];
    memset(s, 0, sizeof(*s));
    s->set = (g_rank + idx) % g_nsets;
    if (open_events(s, NEV(s)) != 0) {    /* leaves s->fd[] = -1 */
        /* keep the slot (fds = -1) so collect() skips it */
        t_open_failed = 1;
        return NULL;
    }
    s->tid = tid;
    read_now(s, s->last);
    t_slot = s;
    return s;
}

/* ── threads that started before counters (OpenMP pools) ─────────────── */

#define CNT_MAX_PENDING 1024
static pid_t g_pending[CNT_MAX_PENDING];
static int   g_npending = 0;

void cnt_thread_hook(void)
{
    if (g_active) { thread_open(); return; }
    int i = __atomic_fetch_add(&g_npending, 1, __ATOMIC_ACQ_REL);
    if (i < CNT_MAX_PENDING) g_pending[i] = my_tid();
}

/* Open groups for remembered threads from the init thread (perf allows
 * counting another thread of the same process). Their reads at region
 * boundaries still use rdpmc from their own thread, via slot_by_tid. */
static void attach_pending(void)
{
    int n = __atomic_load_n(&g_npending, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_PENDING) n = CNT_MAX_PENDING;
    pid_t self = my_tid();
    for (int i = 0; i < n; i++) {
        pid_t tid = g_pending[i];
        if (tid == self || slot_by_tid(tid)) continue;
        int idx = __atomic_fetch_add(&g_nslots, 1, __ATOMIC_ACQ_REL);
        if (idx >= CNT_MAX_THREADS) { __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED); return; }
        thread_slot_t *s = &g_slots[idx];
        memset(s, 0, sizeof(*s));
        s->set = (g_rank + idx) % g_nsets;
        if (open_events_pid(s, NEV(s), tid) != 0) continue;   /* thread gone: skip */
        s->tid = tid;
        group_read_t g;
        if (read_group(s, &g) == 0)
            for (int e = 0; e < NEV(s); e++) s->last[e] = g.v[e];
    }
}

/* ── pthread_create interposer: threads started after init count from
 *    their first instruction ─────────────────────────────────────────── */

typedef struct { void *(*fn)(void *); void *arg; } tramp_t;

static void *trampoline(void *p)
{
    tramp_t t = *(tramp_t *)p;
    free(p);
    thread_open();
    return t.fn(t.arg);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "pthread_create");
    if (!g_active) return real(th, attr, fn, arg);
    tramp_t *t = malloc(sizeof(*t));
    if (!t) return real(th, attr, fn, arg);
    t->fn = fn;
    t->arg = arg;
    int rc = real(th, attr, trampoline, t);
    if (rc != 0) free(t);
    return rc;
}

/* ── init / stop ─────────────────────────────────────────────────────── */

static void spin_us(int us)
{
    uint64_t end = now_ns() + (uint64_t)us * 1000;
    volatile uint64_t x = 0;
    while (now_ns() < end) x++;
}

static int read_int_file(const char *path, int def)
{
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

/* Fit one set: the full event list, then trim optional events from the
 * end until the group opens AND gets scheduled (probed on the calling
 * thread). Returns the number of events that fit, or 0 with *err set
 * (errno of a required event, or 0 = opened but never scheduled). */
static int fit_set(int set, int *err)
{
    const cnt_group_t *grp = g_sets[set];
    *err = 0;
    for (int n = grp->n_events; n >= grp->n_required; n--) {
        g_set_nev[set] = n;
        thread_slot_t probe;
        memset(&probe, 0, sizeof(probe));
        probe.set = set;
        g_fail_index = -1;
        int rc = open_events(&probe, n);
        if (rc != 0) {
            *err = -rc;
            if (g_fail_index >= 0 && g_fail_index < grp->n_required) return 0;
            continue;                         /* optional event did not fit */
        }
        spin_us(2000);
        group_read_t g;
        int ok = read_group(&probe, &g) == 0 && g.time_running > 0;
        close_slot(&probe, n);
        if (ok) return n;
        *err = 0;                             /* opened but never scheduled */
    }
    return 0;
}

static char g_set_note[160] = "";

/* ── logical events ──────────────────────────────────────────────────────
 * Weighted parts of one kind (Intel FLOPs: FP_ARITH_INST_RETIRED by
 * width) become one logical event, so the rest of miniprof sees the same
 * groups on every CPU. Counting stays per physical event; the counts are
 * folded at collect and region read-out. Without weighted events the
 * logical group is the physical one. */
static cnt_group_t g_lsets[CNT_MAX_SETS];
static int         g_lnev[CNT_MAX_SETS];
static int         g_lmap[CNT_MAX_SETS][CNT_MAX_EVENTS];   /* physical -> logical */
static uint64_t    g_lw[CNT_MAX_SETS][CNT_MAX_EVENTS];     /* weight of each physical */

static void build_logical(int k)
{
    const cnt_group_t *g = g_sets[k];
    cnt_group_t *l = &g_lsets[k];
    memset(l, 0, sizeof(*l));
    l->uarch = g->uarch; l->group = g->group; l->no_rdpmc = g->no_rdpmc;
    int nl = 0, active = 0, req = 0;
    for (int p = 0; p < g->n_events; p++) {
        const cnt_event_t *e = &g->ev[p];
        int to = -1;
        if (e->weight)
            for (int q = 0; q < p && to < 0; q++)
                if (g->ev[q].weight && g->ev[q].kind == e->kind) to = g_lmap[k][q];
        if (to < 0) {
            to = nl++;
            l->ev[to] = *e;
            if (p < g_set_nev[k]) active++;
            if (p < g->n_required) req++;
        }
        g_lmap[k][p] = to;
        g_lw[k][p] = e->weight ? e->weight : 1;
    }
    l->n_events = nl;
    l->n_required = req;
    g_lnev[k] = active;
}

static void to_logical(int k, const uint64_t *phys, uint64_t *out)
{
    memset(out, 0, sizeof(uint64_t) * CNT_MAX_EVENTS);
    for (int p = 0; p < g_set_nev[k]; p++) out[g_lmap[k][p]] += g_lw[k][p] * phys[p];
}

int cnt_init(const char *uarch, const char *group, int rank)
{
    g_regions_on = 1;
    g_rank = rank < 0 ? 0 : rank;
    g_pagesize = sysconf(_SC_PAGESIZE);
    const char *ct = getenv("MINIPROF_COUNTERS_TEST");
    const char *ua = !ct ? uarch : strcmp(ct, "tma") == 0 ? "swtest_tma" : "swtest";
    g_use_rdpmc = HAVE_RDPMC &&
                  read_int_file("/sys/bus/event_source/devices/cpu/rdpmc", 1) != 0 &&
                  !getenv("MINIPROF_NO_RDPMC");

    g_nsets = 1;
    if (group && strcmp(group, "topdown") == 0) {
        static const char *names[CNT_MAX_SETS] = { "td_l1", "td_febe", "td_rbs" };
        int n = 0;
        const cnt_group_t *pm = find_group(ua, "td_metrics");   /* Intel: one set */
        if (pm) g_sets[n++] = pm;
        else
            for (int k = 0; k < CNT_MAX_SETS; k++) {
                const cnt_group_t *g = find_group(ua, names[k]);
                if (g) g_sets[n++] = g;
            }
        if (n == 0) {   /* no top-down tables for this CPU: base group */
            snprintf(g_set_note, sizeof(g_set_note),
                     "no top-down event tables for uarch '%s'", ua ? ua : "?");
            g_sets[0] = cnt_lookup(ua, "base");
        } else {
            g_nsets = n;
        }
    } else {
        g_sets[0] = cnt_lookup(ua, group);
    }

    /* Fit every set. The first is required; a later set that cannot be
     * scheduled is dropped from the rotation (its L2 metrics go missing). */
    g_active = 1;
    int last_err = 0, kept = 0;
    for (int k = 0; k < g_nsets; k++) {
        int err = 0;
        int nfit = fit_set(k, &err);
        if (nfit == 0) {
            if (k == 0) { last_err = err; kept = 0; break; }
            snprintf(g_set_note, sizeof(g_set_note), "set %s/%s could not be scheduled%s%s",
                     g_sets[k]->uarch, g_sets[k]->group, err ? ": " : "", err ? strerror(err) : "");
            continue;
        }
        g_sets[kept] = g_sets[k];
        g_set_nev[kept] = nfit;
        kept++;
    }
    if (kept == 0) {
        const cnt_group_t *g0 = g_sets[0];
        g_active = 0;
        g_nsets = 1;
        if (last_err)
            snprintf(g_reason, sizeof(g_reason),
                     "perf_event_open failed for %s/%s: %s%s", g0->uarch,
                     g0->group, strerror(last_err),
                     (last_err == EACCES || last_err == EPERM)
                         ? " (check /proc/sys/kernel/perf_event_paranoid)" :
                     (last_err == ENOENT || last_err == ENODEV || last_err == EOPNOTSUPP)
                         ? " (no PMU: virtual machine or unsupported CPU?)" : "");
        else
            snprintf(g_reason, sizeof(g_reason),
                     "%s/%s: even the %d required events were never scheduled on the "
                     "PMU: counters in use (NMI watchdog, another profiler)?",
                     g0->uarch, g0->group, g0->n_required);
        return 0;
    }
    g_nsets = kept;
    for (int k = 0; k < g_nsets; k++) build_logical(k);

    g_main_slot = thread_open();
    if (!g_main_slot) {
        g_active = 0;
        snprintf(g_reason, sizeof(g_reason), "could not open counters on the main thread");
        return 0;
    }
    attach_pending();
    const char *split = getenv("MINIPROF_COUNTERS_SPLIT");
    g_split = !(split && strcmp(split, "0") == 0);
    return 1;
}

const char *cnt_set_note(void) { return g_set_note; }

void cnt_stop(void)
{
    g_regions_on = 0;
    /* g_active stays set until collect: boundary reads become no-ops via
     * the mpi_trace mode switch, and threads still running keep counting
     * so the final read sees their full totals. */
}

/* ── MPI boundaries ──────────────────────────────────────────────────── */

static inline void switch_bucket(thread_slot_t *s, int to)
{
    uint64_t v[CNT_MAX_EVENTS];
    if (read_now(s, v) != 0) return;
    for (int i = 0; i < NEV(s); i++) {
        s->acc[s->bucket][i] += v[i] - s->last[i];
        s->last[i] = v[i];
    }
    s->bucket = to;
}

static inline void boundary(int entering_mpi)
{
    thread_slot_t *s = t_slot ? t_slot : thread_open();
    if (!s) return;
    /* leaving MPI: an OpenMP worker can only be inside a region here */
    switch_bucket(s, entering_mpi ? CB_MPI : CB_COMPUTE);
}

void cnt_mpi_enter(void) { if (g_active && g_split) boundary(1); }
void cnt_mpi_exit(void)  { if (g_active && g_split) boundary(0); }

/* OpenMP worker threads: after a parallel region they wait at its closing
 * barrier and then in the runtime's thread pool, spinning for a while
 * (KMP_BLOCKTIME, 200 ms by default in LLVM's runtime) before sleeping.
 * Counted as compute, that spin inflates instructions and lowers IPC
 * whenever the master thread is in MPI or serial code. A worker counts as
 * compute from its implicit task's begin to the start of the region's
 * closing barrier wait, and as CB_IDLE from there to its next task. (LLVM
 * reports the worker's barrier-wait end and implicit-task end only at the
 * next fork, so those cannot mark the switch.) The MPI-calling thread is
 * left alone: its time outside regions is serial work. */
void cnt_omp_worker(int working)
{
    if (!g_active || !g_split) return;
    thread_slot_t *s = t_slot ? t_slot : thread_open();
    if (!s || s == g_main_slot) return;
    s->omp_worker = 1;
    if (working && s->bucket == CB_IDLE) switch_bucket(s, CB_COMPUTE);
    else if (!working && s->bucket == CB_COMPUTE) switch_bucket(s, CB_IDLE);
}

int cnt_split_enabled(void) { return g_split; }

/* ── regions ─────────────────────────────────────────────────────────── */

static cnt_region_t g_regions[CNT_MAX_REGIONS];
static int g_nregions = 0;
static pthread_mutex_t g_region_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_region_warned = 0;

static int region_id(const char *name)
{
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (strncmp(g_regions[i].name, name, CNT_REGION_NAME - 1) == 0) return i;
    pthread_mutex_lock(&g_region_lock);
    n = g_nregions;
    for (int i = 0; i < n; i++)
        if (strncmp(g_regions[i].name, name, CNT_REGION_NAME - 1) == 0) {
            pthread_mutex_unlock(&g_region_lock);
            return i;
        }
    int id = -1;
    if (n < CNT_MAX_REGIONS) {
        snprintf(g_regions[n].name, CNT_REGION_NAME, "%s", name);
        id = n;
        __atomic_store_n(&g_nregions, n + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_region_lock);
    return id;
}

/* Per-thread stack for threads that have no counter slot (counters
 * unavailable): regions still get calls and time. */
static _Thread_local region_frame_t t_nostack[CNT_REGION_DEPTH];
static _Thread_local int t_nodepth = 0;

void miniprof_region_begin(const char *name)
{
    if (!g_regions_on || !name) return;
    int id = region_id(name);
    if (id < 0) return;
    thread_slot_t *s = g_active ? (t_slot ? t_slot : thread_open()) : NULL;
    region_frame_t *f;
    if (s) {
        if (s->depth >= CNT_REGION_DEPTH) return;
        f = &s->stack[s->depth++];
        if (read_now(s, f->v0) != 0) memset(f->v0, 0, sizeof(f->v0));
    } else {
        if (t_nodepth >= CNT_REGION_DEPTH) return;
        f = &t_nostack[t_nodepth++];
    }
    f->region = id;
    f->t0_ns = now_ns();
}

void miniprof_region_end(const char *name)
{
    if (!g_regions_on) return;
    uint64_t t1 = now_ns();
    thread_slot_t *s = g_active ? t_slot : NULL;
    region_frame_t *f;
    uint64_t v[CNT_MAX_EVENTS];
    int have_v = 0;
    if (s) {
        if (s->depth <= 0) return;
        f = &s->stack[--s->depth];
        have_v = read_now(s, v) == 0;
    } else {
        if (t_nodepth <= 0) return;
        f = &t_nostack[--t_nodepth];
    }
    cnt_region_t *r = &g_regions[f->region];
    if (name && strncmp(r->name, name, CNT_REGION_NAME - 1) != 0 &&
        !__atomic_exchange_n(&g_region_warned, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "[miniprof] region_end(\"%s\") closes \"%s\": "
                        "regions must nest; using the innermost\n", name, r->name);
    __atomic_fetch_add(&r->calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&r->ns, t1 - f->t0_ns, __ATOMIC_RELAXED);
    if (have_v)
        for (int i = 0; i < NEV(s); i++)
            __atomic_fetch_add(&r->ev[s->set][i], v[i] - f->v0[i], __ATOMIC_RELAXED);
}

/* Fortran: CALL MINIPROF_REGION_BEGIN('name') — hidden length, blank-padded */
static void f_name(char *dst, const char *src, size_t len)
{
    while (len > 0 && src[len - 1] == ' ') len--;
    if (len >= CNT_REGION_NAME) len = CNT_REGION_NAME - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}
void miniprof_region_begin_(const char *name, size_t len)
{
    char buf[CNT_REGION_NAME];
    f_name(buf, name, len);
    miniprof_region_begin(buf);
}
void miniprof_region_end_(const char *name, size_t len)
{
    char buf[CNT_REGION_NAME];
    f_name(buf, name, len);
    miniprof_region_end(buf);
}

int cnt_regions_get(cnt_region_t *out, int max)
{
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].name, g_regions[i].name, CNT_REGION_NAME);
        out[i].calls = __atomic_load_n(&g_regions[i].calls, __ATOMIC_RELAXED);
        out[i].ns    = __atomic_load_n(&g_regions[i].ns, __ATOMIC_RELAXED);
        memset(out[i].ev, 0, sizeof(out[i].ev));
        for (int k = 0; k < CNT_MAX_SETS; k++) {
            uint64_t phys[CNT_MAX_EVENTS];
            for (int e = 0; e < CNT_MAX_EVENTS; e++)
                phys[e] = __atomic_load_n(&g_regions[i].ev[k][e], __ATOMIC_RELAXED);
            if (k < g_nsets) to_logical(k, phys, out[i].ev[k]);
        }
    }
    return n;
}

/* ── collect ─────────────────────────────────────────────────────────── */

void cnt_collect(cnt_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->group = g_active ? &g_lsets[0] : g_sets[0];
    out->nsets = g_nsets;
    for (int k = 0; k < g_nsets; k++) {
        out->set_group[k] = g_active ? &g_lsets[k] : g_sets[k];
        out->set_nev[k] = g_active ? g_lnev[k] : g_set_nev[k];
    }
    out->rdpmc = g_use_rdpmc;
    out->threads_dropped = __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
    if (!g_active) {
        out->available = 0;
        snprintf(out->reason, sizeof(out->reason), "%s",
                 g_reason[0] ? g_reason : "counters not initialised");
        return;
    }
    out->available = 1;
    out->n_active = g_lnev[0];
    /* per physical event, folded into logical events at the end */
    static uint64_t pc[CNT_MAX_SETS][CNT_MAX_EVENTS], pm[CNT_MAX_SETS][CNT_MAX_EVENTS],
                    pi[CNT_MAX_SETS][CNT_MAX_EVENTS], pmain[CNT_MAX_EVENTS];
    memset(pc, 0, sizeof(pc)); memset(pm, 0, sizeof(pm)); memset(pi, 0, sizeof(pi));
    memset(pmain, 0, sizeof(pmain));

    int n = __atomic_load_n(&g_nslots, __ATOMIC_ACQUIRE);
    if (n > CNT_MAX_THREADS) n = CNT_MAX_THREADS;
    for (int t = 0; t < n; t++) {
        thread_slot_t *s = &g_slots[t];
        if (s->fd[0] < 0) continue;
        /* Final values via read(): works for threads that already exited
         * (the event keeps its final count) and for other threads (rdpmc
         * would read THIS thread's counters). */
        group_read_t g;
        if (read_group(s, &g) != 0) continue;
        if (g.time_running < g.time_enabled - g.time_enabled / 100) out->multiplexed = 1;
        out->threads++;
        out->set_threads[s->set]++;
        out->reads_rdpmc   += s->n_rdpmc;
        out->reads_syscall += s->n_syscall;
        for (int i = 0; i < NEV(s); i++) {
            uint64_t tail = g.v[i] - s->last[i];
            uint64_t comp = s->acc[CB_COMPUTE][i] + (s->bucket == CB_COMPUTE ? tail : 0);
            uint64_t mpi  = s->acc[CB_MPI][i]     + (s->bucket == CB_MPI ? tail : 0);
            uint64_t idle = s->acc[CB_IDLE][i]    + (s->bucket == CB_IDLE ? tail : 0);
            pc[s->set][i] += comp;
            pm[s->set][i] += mpi;
            pi[s->set][i] += idle;
            if (s == g_main_slot && s->set == 0) pmain[i] = comp + mpi + idle;
        }
    }
    for (int k = 0; k < g_nsets; k++) {
        to_logical(k, pc[k], out->set_compute[k]);
        to_logical(k, pm[k], out->set_mpi[k]);
        to_logical(k, pi[k], out->set_idle[k]);
        for (int e = 0; e < g_lnev[k]; e++)
            out->set_total[k][e] = out->set_compute[k][e] + out->set_mpi[k][e] + out->set_idle[k][e];
    }
    to_logical(0, pmain, out->main_thread);
    memcpy(out->total,   out->set_total[0],   sizeof(out->total));
    memcpy(out->compute, out->set_compute[0], sizeof(out->compute));
    memcpy(out->mpi,     out->set_mpi[0],     sizeof(out->mpi));
    memcpy(out->idle,    out->set_idle[0],    sizeof(out->idle));
    g_active = 0;   /* no more boundary reads */
}
