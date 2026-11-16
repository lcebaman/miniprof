# Snapshot reports

A snapshot is a whole-run characterisation in the style of Intel APS and
AMDuProfPcm. It records no individual samples or calls: each rank keeps
running totals, and at `MPI_Finalize` they are gathered into one JSON
document. The text report, the HTML report, the CSV tables and
`miniprof-diff` are all generated from that file.

This page follows the report from top to bottom.

- [Run header](#run-header)
- [Time split and imbalance](#time-split-and-imbalance)
- [CPU binding and utilisation](#cpu-binding-and-utilisation)
- [MPI](#mpi)
- [I/O](#io)
- [OpenMP](#openmp)
- [Hotspots](#hotspots)
- [Hardware counters](#hardware-counters)
- [Top-down](#top-down)
- [Memory](#memory)
- [Roofline](#roofline)
- [Regions](#regions)
- [Observations and thresholds](#observations-and-thresholds)
- [Profiler overhead](#profiler-overhead)
- [The JSON document](#the-json-document)

A real text report, from the bundled `diffusion3d` benchmark on 4 ranks
with `--hotspots` (on a machine without hardware counters, so those
sections say why they are missing):

```
==============================================================================
 miniprof snapshot  |  /home/user/miniprof/examples/diffusion3d
 4 ranks on 1 node(s)  |  Skylake/Cascade Lake SP  |  2026-11-16T18:55:07Z
==============================================================================
 Elapsed time          20.46 s  (slowest rank)

 Time split (average over ranks)
   Compute      92.7%  ############################..
   MPI           7.3%  ##............................
   I/O           0.0%  ..............................

 Compute imbalance       0.8%   (perfect balance would save 0.8% of elapsed)
   slowest ranks       1 (19.11 s), 0 (19.02 s), 3 (18.87 s)
 MPI time imbalance      7.7%   (min 1.34 s, max 1.62 s)

 MPI by class           time%    calls        data
   p2p                   7.0%      3200   400.0 MiB
   collective            0.3%       184   513.4 KiB

 Top MPI functions      time%    imb%    calls   avg size
   MPI_Sendrecv          7.0%    5.4%     3200  128.0 KiB
   MPI_Allreduce         0.3%   42.0%      176        8 B

 Message sizes
   p2p          most calls: 128.0 KiB-256.0 KiB (100%);  most time: 128.0 KiB-256.0 KiB
   collective   most calls: 8 B-16 B (96%);  most time: 8 B-16 B

 Hotspots: CPU time sampled every 10 ms (4,045 samples, 40.45 s); time in the function itself
   USER 91.7%  LIBC 8.1%  MPI 0.2%
    CPU%  CPU/rank  imbal  group  function  [library]
    82.4    8.34 s     3%  USER   main  [diffusion3d]
     9.1  925.0 ms     2%  USER   apply_bc  [diffusion3d]
     4.4  442.5 ms    22%  LIBC   sched_yield  [libc.so.6]
 ...
 Hardware counters      unavailable: perf_event_open failed for generic/base: No such file or directory (no PMU: virtual machine or unsupported CPU?)
 Memory (node)          not measured: no node-level memory counters for uarch 'skylake_sp' (AMD Zen 4 / 5 / 6 only)
 Roofline               not available: no calibration: run miniprof-calibrate once per node type and set MINIPROF_CALIBRATION
 OpenMP                 not measured: application does not use OpenMP
```

A section that cannot be measured always says why; the rest of the report
is unaffected.

## Run header

Command, rank and node counts, ranks per node, CPUs each rank may run on,
OpenMP threads, the CPU microarchitecture (read from CPUID: `zen4`,
`zen5`, `granite_rapids`...), MPI library and thread level, kernel and
`perf_event_paranoid`. The JSON's `nodes` list adds per-node details and an
`exclusivity` hint: whether something else was running on the node.

## Time split and imbalance

Each rank's elapsed time (`MPI_Init` to `MPI_Finalize`) is split into
**MPI** (time inside wrapped MPI calls), **I/O** (time inside wrapped I/O
calls) and **compute** (the rest). Shown as min/avg/max over ranks.

- **Compute imbalance** = (max − avg) / max of compute time. "Perfect
  balance would save X% of elapsed" is how much shorter the run would be if
  every rank computed for the average time.
- **Slowest ranks** name the ranks to look at.
- In an SPMD code, MPI time is often ranks waiting for the slowest one: a
  large compute imbalance with a large MPI share usually means the MPI
  time is load imbalance, not slow communication.

## CPU binding and utilisation

CPU utilisation = CPU time ÷ (elapsed × physical cores the rank may run
on). When ranks may run on many more CPUs than threads they use (the
whole node, typically: no binding), utilisation would read close to 0%
and mean nothing, so the report shows **CPU binding: none** (or `wide`
when bound to e.g. a whole socket), with the CPUs allowed and the number
each rank keeps busy. Unbound ranks can be moved between cores and
sockets by the OS, which shows up as remote memory reads.

## MPI

- **By class**: point-to-point, collective, wait/test, RMA; time, calls,
  bytes.
- **By function**: share of the run, imbalance across ranks, calls and
  average message size. Imbalance here is (max − avg) / max of each
  function's time.
- **Message sizes**: power-of-two histograms per class, by calls and by
  time. A collective spending its time on 8-byte messages is latency
  bound; one on megabyte messages is bandwidth bound.

The wrapped calls are listed in [limitations](limitations.md). An
unwrapped MPI call's time counts as compute.

## I/O

POSIX and MPI-IO are reported separately: calls, bytes, and time for
reads, writes and metadata (open, close, sync, set_view, set_size...).

- POSIX covers `open/read/write/pread/pwrite/fread/fwrite/fsync`, text
  stdio (`fgets/getline/fscanf/fprintf/fputs`) and their
  `_FORTIFY_SOURCE` variants.
- MPI-IO covers blocking, collective and nonblocking reads and writes, and
  the calls PnetCDF makes around them (`set_view`, `set_size`,
  `preallocate`, `set_info`).
- Only files the application opens on a regular path are counted. The MPI
  library's own I/O is excluded, and POSIX calls made underneath MPI-IO are
  not counted twice.

## OpenMP

Measured through the OMPT tools interface, so it needs LLVM `libomp`
(clang, AOCC) or Intel `libiomp5`. miniprof registers itself as the OMPT
tool automatically.

- **Serial time**: time on the initial thread outside any parallel region.
  The report also gives **serial compute**: serial time minus MPI and I/O,
  which happen outside regions in most codes. This is the share of the run
  where only one thread per rank works.
- **Thread time** split into work, barrier wait (explicit barriers and the
  implicit ones at the end of loops and regions), lock wait (`critical`,
  `omp_set_lock`...) and taskwait.
- **Imbalance** = barrier wait ÷ team thread time, overall and per
  parallel region. Regions are named after the function containing them,
  with file:line when the binary has `-g`, and reduced across ranks.
- The first region of a run includes the runtime creating its worker
  threads, so it can show imbalance even when its work is balanced.

**OpenMP workers idle.** Between parallel regions, worker threads wait in
the runtime's thread pool, spinning for a while (`KMP_BLOCKTIME`, 200 ms
by default in LLVM's runtime) before sleeping. Their counters during that
time go to a separate `omp_idle` bucket, not compute, so the compute IPC
describes real work. The report gives the idle share of cycles and the
idle core-seconds per rank. Above 20% the observations suggest
`OMP_WAIT_POLICY=passive` or a lower `KMP_BLOCKTIME`, which give that CPU
time (and power) back at some cost in region start-up latency. Explicit
tasks that workers run while waiting at a barrier count as compute.

## Hotspots

With `--hotspots` (or `MINIPROF_HOTSPOTS=1`), every thread is sampled on
CPU time every 10 ms (`MINIPROF_HOTSPOTS_INTERVAL_MS`) and each sample
counts in the function it interrupted. At `MPI_Finalize` each rank names
its functions in-process from the binaries' symbol tables and sends its
top 48; the report shows the top 30 across ranks:

- the split by group: application (`USER`), MPI library, OpenMP runtime
  (`OMP`), other libraries;
- per function: share of sampled CPU time, CPU seconds per rank,
  imbalance across ranks, library.

How to read it:

- Times are **exclusive**: a routine that mostly calls others ranks low.
- Waiting in MPI shows as MPI library code (`opal_progress`,
  `[mca_btl_vader.so]`), waiting in OpenMP as runtime code. Libraries
  without function names show as `[library.so]`.
- Names come from the symbol table, so **inlined functions appear as their
  caller**: in the example above, `diffusion3d`'s stencil kernels are
  inlined into `main` at `-O2`. The sampling profile (`--sample`) reads the
  debug information instead and names them (`compute_diffusion` 18%,
  `compute_viscous_dissipation` 18%...) with source lines; it needs `-g`.
- Each rank sends its top 48 functions; time in functions below that is
  shown as "minor functions" so the groups add up.

Hotspots are off by default. Their cost was within run-to-run noise on
LAMMPS.

## Hardware counters

Counted per thread, in user space (works at `perf_event_paranoid = 2`),
including threads created after `MPI_Init`, and split at every MPI call
into outside-MPI (compute) and inside-MPI. Reads use `rdpmc` when allowed
(about 20 ns), `read()` otherwise.

At the `basic` level:

| Metric | Meaning |
|---|---|
| IPC (overall, compute, inside MPI) | instructions per cycle; compute IPC is the one that describes your code |
| effective clock | cycles ÷ busy time |
| GFLOP/s, FLOPs per cycle | from the FP-operation counter (all precisions together) |
| L2 misses per 1000 instructions | cache behaviour |
| DRAM fill rate, remote share | L1D fills from DRAM and from the other socket; a lower bound (lines brought in by the L2 prefetcher are not seen) |
| FLOPs inside MPI | should be ~0; a check of the split |

Which events are available depends on the CPU and on counters taken by
the NMI watchdog; see [hardware support](hardware.md). Events that do not
fit are dropped from the end of the group, and the JSON says which.

## Top-down

At `--level topdown` (AMD Zen 4/5/6), AMD's pipeline-utilisation model,
the one behind `perf stat -M PipelineL1,PipelineL2`, as a share of
dispatch slots:

| Level 1 | Level 2 | What to do about it |
|---|---|---|
| frontend bound | latency, bandwidth | instruction cache and branch target misses: code layout, PGO |
| bad speculation | mispredicts, pipeline restarts | branchy code: data-dependent branches, sort or vectorise |
| backend bound | memory, core | memory: data layout, blocking, prefetching; core: dependency chains, divisions |
| retiring | fast path, microcode | useful work; if high and the run is still slow, do less work or vectorise |

- Reported for compute, inside MPI (mostly the library polling) and per
  user region.
- On Zen, the refetch bubble after a mispredict shows as frontend latency,
  so very branchy code can look frontend bound. Level 1 can sum to less
  than 100% in strongly latency-bound code.
- Zen 4 and Zen 5 can classify the same stall differently; compare
  top-down profiles within one CPU generation.
- The 12 events do not fit in one thread's counters, so they are split
  into three sets, and each thread counts one set for the whole run
  (thread *i* of rank *r* counts set `(r + i) mod 3`). Every ratio is formed
  within one set, so nothing is multiplexed or scaled. This assumes ranks
  and threads run similar code; level 2 needs at least 3 counted threads.
- `topdown` replaces the `basic` group for that run: no IPC or FLOPs.

## Memory

Node-level counters, per node and per socket (AMD Zen 4/5/6, any level,
needs the [memory helper](installing.md#the-memory-helper)):

- DRAM read and write bandwidth, average and peak (best 100 ms interval),
  per channel, and how many channels carry traffic;
- the share of each socket's reads served by the other socket (NUMA
  placement: high with unbound ranks or first-touch on the wrong socket);
- package energy and average power, and energy to solution;
- with a calibration, each as a % of what the node can sustain.

These counters see the **whole node**, not just the job: the node's
`exclusivity` hint says whether something else was running.

## Roofline

With a [calibration](installing.md#calibration), each node's run is
placed on a roofline: FLOP rate against arithmetic intensity (FLOPs per
DRAM byte), under the calibrated DRAM bandwidth and peak FLOP ceilings.

- **memory-bound / compute-bound** says which side of the ridge point the
  run is on. When it reaches less than half of what that side allows, the
  report says **below both roofs**: neither bandwidth nor peak FLOPs is
  the limit, and the cause is latency, the core itself (see top-down), or
  time spent outside compute.
- The compute roof is the widest FP64 rate unless
  `MINIPROF_ROOFLINE_PEAK` names another (`fp32_512`...), because the FLOP
  counter does not separate precisions. Widths within 3% of the peak
  (`fp64_256` and `fp64_512` on Zen 4) are drawn as one roof.
- Without the memory block, the roofline is an **estimate** from core
  counters, which see only reads and miss some of them; the point then sits
  to the right of where it belongs.
- Ceilings are measurements, not datasheet figures: a well-tuned kernel
  can land a few percent above them.
- At `--level roofline`, each [region](#regions) also gets a point, using
  FLOPs per DRAM byte *read* (fills plus L2-prefetch misses) against the
  read-only ceiling.

## Regions

Code bracketed with `miniprof_region_begin/end` ([user
guide](user-guide.md#regions-timing-parts-of-your-code)) gets its own
calls, time, counters, top-down and roofline point, reduced across ranks.

## Observations and thresholds

The report ends with findings, each triggered by a threshold, for example:

```
 Observations
   ! MPI takes 24.8% of the run (> 20%); largest contributor MPI_Allreduce.
   ! Compute is imbalanced across ranks (25.0% > 10%); much of the MPI wait time is likely ranks waiting for the slowest ones.
```

The thresholds can be changed per run or per benchmark; see
[comparing runs](comparing-runs.md#thresholds).

## Profiler overhead

The run table shows miniprof's own start-up and shut-down cost, and which
phases took longest. It is typically a few tens of milliseconds in total.
The cost while the application runs (about 95 ns per wrapped MPI call, a
counter read at each MPI call) does not appear there; measure it by
comparing the application's own timer with and without miniprof.

## The JSON document

`snapshot.json` is versioned (`"schema": "miniprof-snapshot"`,
`schema_version`) and stable enough to script against. Top level:

| Key | Contents |
|---|---|
| `run` | command, ranks, nodes, threads, elapsed (min/avg/max), MPI library |
| `nodes`, `ranks` | per-node and per-rank details |
| `metrics.collected` | the blocks this run collected (`parallel`, `counters`, `openmp`, `hotspots`...) |
| `metrics.parallel` | time split, MPI, I/O, CPU |
| `metrics.openmp` | OpenMP |
| `metrics.counters` | counter totals (`totals = compute + mpi + omp_idle`), derived metrics, regions |
| `metrics.topdown` | top-down |
| `metrics.memory` | memory and energy |
| `metrics.roofline` | roofline per node and per region |
| `metrics.hotspots` | hotspots, when requested |
| `overhead` | miniprof's own cost, by phase |

Every block has `available`, and a `reason` when it is false.
`miniprof-csv` (or `miniprof-report --csv DIR`) flattens the tables into
CSV files for spreadsheets and plotting.
