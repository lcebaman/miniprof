# Hardware support

Everything that is not a CPU or node counter (time split, MPI, I/O,
OpenMP, hotspots, sampling, tracing) works on any x86-64 Linux. The
hardware sections depend on the CPU and on permissions.

## What each CPU gets

| CPU | Core counters | Top-down | Memory and energy | Roofline |
|---|---|---|---|---|
| AMD Zen 4 (Genoa, Bergamo, Siena) | IPC, FLOPs, DRAM fills, L2 misses, FP width mix | yes (6 slots/cycle) | yes (data fabric) | yes |
| AMD Zen 5 (Turin) | as Zen 4 | yes (8 slots/cycle) | yes (memory controllers) | yes |
| AMD Zen 6 (Venice) | as Zen 5 (same encodings, checked against perf) | yes | node 0 only on the RHEL 9 kernel (see below) | estimate unless memory covers every node |
| Intel (Skylake SP to Granite Rapids) and other CPUs | cycles and instructions (IPC) | no | no | no |

The microarchitecture is read from CPUID and shown in the report header
and the JSON (`uarch`).

## Permissions

| Feature | Needs |
|---|---|
| core counters, top-down | `perf_event_paranoid <= 2` (counts are user space only) |
| `rdpmc` fast reads | `/sys/bus/event_source/devices/cpu/rdpmc` allowing it; otherwise `read()`, slower per MPI call |
| memory and energy | the helper with `CAP_PERFMON`, or `perf_event_paranoid <= 0` ([installing](installing.md#the-memory-helper)) |

Without them, the section says why and the rest of the report is
unaffected. In a VM without a virtual PMU there are no counters at all.

## Core counter groups

Each thread has a few programmable counters, so each run collects one
group, chosen by the level:

| Level | Group | Events |
|---|---|---|
| `basic` | `base` | cycles, instructions, FLOPs (takes two counters), plus if they fit: L1D fills from DRAM, from the remote socket, L2 misses |
| `roofline` | `roofline` | cycles, FLOPs, L1D fills from DRAM, L2-prefetcher requests that miss L2 and L3 |
| `topdown` | three sets | 12 pipeline events, one set per thread (see [snapshot reports](snapshot.md#top-down)) |
| `full` | `vector` | FP operations by width (scalar, 128, 256, 512-bit), cycles |

Zen has 6 general-purpose counters per thread. The NMI watchdog, on by
default on RHEL-family systems, takes one; events that no longer fit are
dropped from the end of the group, and the JSON lists them. Counts are per
thread, including threads created after `MPI_Init`.

Notes:

- The FLOP counter counts all precisions together, so GFLOP/s mixes FP32
  and FP64 operations.
- DRAM fills at `basic` are a lower bound: a line the L2 prefetcher brings
  in is filled into L1 from L2, not from DRAM. The `roofline` group adds
  the prefetcher's DRAM requests; on an EPYC 9754 the two together saw
  99.9% of the reads of five known kernels.
- The FP width events count every FP-unit operation, including loads,
  stores and moves, so the width mix shows vector instruction usage rather
  than the vectorised share of arithmetic.

## Top-down on Zen

- Bad speculation counts only squashed operations; the refetch bubble after
  a mispredict appears as frontend latency.
- Zen 4 and Zen 5 can classify the same stall differently (a dependent FP
  chain is backend-core bound on Zen 4 but mostly frontend latency on Zen
  5); `perf stat -M PipelineL1,PipelineL2` agrees on both. Compare within
  one generation.
- Level 1 can sum to less than 100% in strongly latency-bound code (about
  85% for a pointer chase).

## Memory

| CPU | Source | Notes |
|---|---|---|
| Zen 5 | memory controllers, one PMU per channel | |
| Zen 4 | data fabric DRAM-channel counters | 32 events share 16 counters per socket, so they are time-multiplexed and scaled (`multiplexed`). The memory-controller PMUs on Zen 4 (RHEL 9) see only half the channels and count each line twice, which also affects perf's `umc_mem_*_bandwidth` metrics; miniprof does not use them |
| Zen 6 | data fabric | On the RHEL 9 kernel the single data-fabric PMU sees only NUMA node 0: the block carries `coverage` saying so, drops the `% of calibrated` figures, and the roofline falls back to the core-counter estimate. Run the job on node 0 to measure it fully |

The counters see the whole node, not just the job. One rank per node runs
the helper; peaks are the best 100 ms interval (`MINIPROF_MEMORY_INTERVAL_MS`).

## Calibration figures

Ceilings are measured, not taken from datasheets. Two examples:

| Node | DRAM (triad) | FP64 peak |
|---|---|---|
| 2 × EPYC 9654 (Genoa, 192 cores, DDR5-4800) | 764 GB/s | 11.15 TFLOP/s (fp64_512 ≈ fp64_256: Zen 4 splits 512-bit operations) |
| 2 × EPYC 9754 (Bergamo, 256 cores) | 752 GB/s streaming (about 82% of the 922 GB/s theoretical) | 12.46 TFLOP/s |

## Probing a node

| Script | Purpose |
|---|---|
| `tools/pmu_probe.sh` | what the node's PMU and perf support |
| `tools/nodemon_ready.sh` | whether the memory helper can work here |
| `tools/validate_counters.sh` | exact region FLOPs, thread coverage and the compute/MPI split against `perf stat` |
| `tools/validate_topdown.sh` | every raw top-down encoding against perf's, and the result against `perf stat -M PipelineL1/L2` |
| `tools/validate_memory.sh` | known triad traffic, and a remote-memory run |
| `tools/validate_roofline.sh` | a streaming run memory-bound at ≥ 50% of the calibrated bandwidth, an FMA run compute-bound at ≥ 60% of peak |
| `tools/probe_fills.sh`, `tools/probe_df_zen6.sh` | event coverage checks used when adding a CPU |

Validated so far on EPYC 9754 (Zen 4c), 9654 (Zen 4), 9555 (Zen 5) and 9996
(Zen 6).
