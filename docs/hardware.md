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
| Intel Granite Rapids | IPC, FLOPs, DRAM reads (demand + prefetch, remote share), L2 misses, FP width mix | yes (Intel TMA, levels 1 and 2) | yes (memory controllers; no remote-socket traffic) | yes |
| Other Intel (Skylake SP to Emerald Rapids) and other CPUs | cycles and instructions (IPC) | no | no | no |

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

| Level | Group | AMD Zen events | Intel Granite Rapids events |
|---|---|---|---|
| `basic` | `base` | cycles, instructions, FLOPs (takes two counters), plus if they fit: L1D fills from DRAM, from the remote socket, L2 misses | cycles, instructions, FLOPs (five events), plus if they fit: DRAM reads, remote DRAM reads, L2 misses |
| `roofline` | `roofline` | cycles, FLOPs, L1D fills from DRAM, L2-prefetcher requests that miss L2 and L3 | cycles, instructions, FLOPs, DRAM reads |
| `topdown` | | 12 pipeline events in three sets, one set per thread (see [snapshot reports](snapshot.md#top-down)) | slots and perf's eight `topdown-*` metric events, levels 1 and 2 in one group |
| `full` | `vector` | FP operations by width (scalar, 128, 256, 512-bit), cycles | FP arithmetic instructions by width, cycles, instructions |

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

## Granite Rapids

Encodings are those of the kernel's `graniterapids` event tables.
`tools/probe_intel.sh` checks each against kernels with a known answer.

- **FLOPs** come from `FP_ARITH_INST_RETIRED`, which counts instructions by
  width and precision, an FMA twice. miniprof counts five combinations and
  weights them by FLOPs per instruction (scalar 1, 128-bit double 2,
  128-bit single and 256-bit double 4, 256-bit single and 512-bit double
  8, 512-bit single 16), as perf's FLOPs metric does; the report shows one
  FLOP count. FP16 and AMX operations are not counted.
- **DRAM reads** (`OCR.READS_TO_CORE.DRAM`) count every line the core
  reads from DRAM, demand and prefetch, local and remote: unlike Zen's L1D
  fills this is the core's whole DRAM read traffic, so it is not a lower
  bound, and per-region roofline points work at `basic` too. Writebacks
  are not counted. The remote share comes from `OCR.READS_TO_CORE.REMOTE_DRAM`.
- **FP width mix** (`full`) counts arithmetic instructions only, an FMA
  twice; Zen's width events also count FP loads, stores and moves, so the
  mixes of the two vendors are not comparable.
- **Top-down** is Intel's TMA through perf's metric events: the slots
  fixed counter and the `PERF_METRICS` register give levels 1 and 2 in one
  group, on every thread, with no set rotation. The frontend is corrected
  for dropped ops (`INT_MISC.UOP_DROPPING`) and bad speculation is the
  remainder, as in perf's TopdownL1/L2. In the report, level 2 is named as
  Intel names it: fetch latency, branch mispredicts, memory/core bound,
  heavy operations. These events cannot be read with `rdpmc`, so every
  read at an MPI call is a system call: slower per MPI call than the
  other levels.
- **Memory** comes from the memory controllers (`uncore_imc_N`, one per
  channel, two sub-channels each): `UNC_M_CAS_COUNT_SCH0/1.RD` and `.WR`,
  64 B per CAS, as perf's `memory_bandwidth_*` metrics; energy from RAPL.
  Traffic between the sockets is not measured, so the memory block has no
  remote share; the core counters give the job's remote share at `basic`.
- 8 general-purpose counters per thread, plus fixed counters for
  instructions, cycles and slots. With the NMI watchdog holding the cycles
  counter, the `base` group may drop L2 misses to fit.

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
| Granite Rapids | memory controllers (`uncore_imc_N`), two sub-channels each | no remote-socket traffic: `remote_read_share_pct` is null; the core counters give the job's remote share |
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
| `tools/probe_fills.sh`, `tools/probe_df_zen6.sh`, `tools/probe_intel.sh` | event coverage checks used when adding a CPU |

Validated so far on EPYC 9754 (Zen 4c), 9654 (Zen 4), 9555 (Zen 5) and 9996
(Zen 6). Granite Rapids support is new and is being validated with
`tools/probe_intel.sh` and the scripts above.

`tools/probe_intel.sh` checks, on an Intel node, every event miniprof's
Intel support uses against kernels with a known FLOP or byte count, and
the top-down group opened exactly as miniprof opens it.
