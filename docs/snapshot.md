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
- [Timeline](#timeline)
- [Hardware counters](#hardware-counters)
- [Top-down](#top-down)
- [Memory](#memory)
- [Roofline](#roofline)
- [Regions](#regions)
- [Fingerprint](#fingerprint)
- [Observations and thresholds](#observations-and-thresholds)
- [Profiler overhead](#profiler-overhead)
- [The JSON document](#the-json-document)

A real text report, from the bundled `diffusion3d` benchmark on 4 ranks
with `--hotspots` (on a machine without hardware counters, so those
sections say why they are missing):

```
==============================================================================
 miniprof snapshot  |  /home/user/miniprof/examples/diffusion3d
 4 ranks on 1 node(s)  |  Emerald Rapids  |  2026-11-17T09:06:47Z
==============================================================================
 Elapsed time          23.51 s  (slowest rank)

 Time split (average over ranks)
   Compute      88.8%  ###########################...
   MPI          11.2%  ###...........................
   I/O           0.0%  ..............................

 Compute imbalance       0.9%   (perfect balance would save 0.8% of elapsed)
   slowest ranks       1 (21.06 s), 3 (20.99 s), 0 (20.85 s)
 MPI time imbalance     10.1%   (min 2.45 s, max 2.94 s)
 CPU utilisation        24.6%   (min 24.5%, max 24.7%; per physical core)

 MPI by class           time%    calls        data
   p2p                  10.6%      3200   400.0 MiB
   collective            0.7%       184   513.4 KiB

 Top MPI functions      time%    imb%    calls   avg size
   MPI_Sendrecv         10.6%   11.9%     3200  128.0 KiB
   MPI_Allreduce         0.7%   21.8%      176        8 B
   MPI_Gatherv           0.0%   40.2%        4  128.0 KiB
   MPI_Gather            0.0%   74.3%        4        4 B

 Message sizes
   p2p          most calls: 128.0 KiB-256.0 KiB (100%);  most time: 128.0 KiB-256.0 KiB
   collective   most calls: 8 B-16 B (96%);  most time: 8 B-16 B

 Hotspots: CPU time sampled every 10 ms (4,618 samples, 46.18 s); time in the function itself
   USER 90.1%  LIBC 9.3%  MPI 0.6%
    CPU%  CPU/rank  imbal  group  function  [library]
    16.6    1.92 s     3%  USER   compute_diffusion  [diffusion3d]
    15.9    1.83 s     5%  USER   compute_viscous_dissipation  [diffusion3d]
    15.0    1.73 s     8%  USER   compute_darcy_advection  [diffusion3d]
     9.9    1.14 s     5%  USER   compute_phase_change  [diffusion3d]
     8.1  932.5 ms     6%  USER   compute_radiation  [diffusion3d]
 ...

 I/O                    read                 write             time (avg/rank)
   POSIX           0 B (     0 calls)        0 B (     0 calls)   0 us
   MPI-IO          0 B (     0 calls)        0 B (     0 calls)   0 us

 Hardware counters      unavailable: perf_event_open failed for generic/base: No such file or directory (no PMU: virtual machine or unsupported CPU?)

 Memory (node)          not measured: no node-level memory counters for uarch 'emerald_rapids' (AMD Zen 4 / 5 / 6, Intel Granite Rapids)

 Roofline               not available: no calibration: run miniprof-calibrate once per node type and set MINIPROF_CALIBRATION

 OpenMP                 not measured: application does not use OpenMP

 Fingerprint            compute 89% not characterised (no hardware counters); MPI 10% bandwidth (large messages)
   Where the time goes (% of elapsed)
     MPI                     11.2%  ###...........................
     Rank imbalance           0.8%  ..............................
     I/O                      0.0%  ..............................
   Use of the node (% of capability)
     CPU utilisation         24.6%  #######.......................
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
its functions from the binaries' symbol tables (in-process) and, for
binaries built with `-g`, reads the innermost function of the addresses in
its leading functions from the debug information, so inlined kernels get
their own names. It sends its top 48; the report shows the top 30 across
ranks:

- the split by group: application (`USER`), MPI library, OpenMP runtime
  (`OMP`), other libraries;
- per function: share of sampled CPU time, CPU seconds per rank,
  imbalance across ranks, library.

How to read it:

- Times are **exclusive**: a routine that mostly calls others ranks low.
- Waiting in MPI shows as MPI library code (`opal_progress`,
  `[mca_btl_vader.so]`), waiting in OpenMP as runtime code. Libraries
  without function names show as `[library.so]`.
- **Inlined functions** are named only when the binary has debug
  information (`-g`). Without it, their time appears under the function
  they were inlined into. `diffusion3d` at `-O2` inlines its stencil
  kernels into `main`: with `-g` the report shows `compute_diffusion` 17%,
  `compute_viscous_dissipation` 17%...; without, `main` 82%. This step runs
  `addr2line` once per binary with debug information, at finalize (about
  10 ms); `MINIPROF_HOTSPOTS_INLINE=0` turns it off.
- Each rank sends its top 48 functions; time in functions below that is
  shown as "minor functions" so the groups add up.

Hotspots are off by default. Their cost was within run-to-run noise on
LAMMPS.

## Timeline

The rest of the snapshot is totals over the run. With `--timeline` (or
`MINIPROF_TIMELINE=1`) it also records how the run changed over time, so
phases, drift and imbalance that moves between ranks show up instead of
being averaged away. From `examples/timeline_validate` on 2 ranks (compute
only, then rank 0 slow, then rank 1 slow):

```
 Timeline               2 ranks, 37 buckets of 50 ms, 0 to 1.9 s left to right
   MPI % (avg)         |▁▁▁▁▁▁▁▁▁▁▁▁▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄ | 0%..47%
   MPI % (max rank)    |▁▁▁▁▁▁▁▁▁▁▁▁████████████▇███████████ | 0%..94%
   MPI % by rank      (taller = more time in MPI)
     rank 0            |▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▇███████████ |
     rank 1            |▁▁▁▁▁▁▁▁▁▁▁▁████████████▁▁▁▁▁▁▁▁▁▁▁▁ |
```

The average alone says "MPI about half the time from 0.6 s on"; the rows
show it is one rank waiting for the other, and that the waiting rank
changes at 1.2 s. The HTML report draws the same series as charts that
share one time axis, with a heatmap of MPI share by rank and time.

What is recorded, per time bucket:

| Series | Per | Source |
|---|---|---|
| MPI and I/O share of the bucket, min/avg/max over ranks | rank | the MPI and I/O wrappers, which already time every call; a call spanning buckets is split |
| MPI share by rank group (heatmap) | up to 64 rows of consecutive ranks | as above |
| IPC, min/avg/max over ranks | the thread that calls MPI | counter reads already made at MPI calls |
| GFLOP/s, summed over ranks | the thread that calls MPI | as above |
| DRAM read/write GB/s and package power | node (up to 16 nodes) | the memory helper's samples |

How to read it:

- Buckets start at 100 ms (`MINIPROF_TIMELINE_INTERVAL_MS`). A run longer
  than 512 buckets doubles the width, merging neighbours, so any length
  fits; the node series do the same with 256 buckets.
- The memory helper samples every 100 ms (`MINIPROF_MEMORY_INTERVAL_MS`);
  its samples are put on the ranks' buckets (time-weighted, from the node
  leader's start), so every series shares one time axis. With buckets
  finer than the helper's samples, the node series is a step every
  sample. The JSON keeps the sample period as `sample_s`.
- The node series cover what the memory block covers: on Zen 6 with the
  RHEL 9 kernel, NUMA node 0 only (see [hardware](hardware.md)).
- Each rank's buckets count from its own start. Ranks leave `MPI_Init` at
  nearly the same time, so this is what you want for phases; it is not a
  clock to compare with external logs.
- IPC and GFLOP/s are those of the thread that calls MPI. In an
  OpenMP code the worker threads are in the run totals but not in the
  timeline. They are also only as fine as the gaps between MPI calls: the
  counts between two MPI calls are spread over the buckets in between in
  proportion to time.
- MPI time is time inside MPI calls, waiting included: a rank waiting for
  a slow one shows as MPI, as in the totals. The heatmap shows which.

The timeline adds a clock read and an atomic add to each MPI and I/O call
(tens of nanoseconds) and a few reductions at `MPI_Finalize`. It is off
by default.

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

At `--level topdown`, a top-down breakdown of the compute pipeline. On AMD
Zen 4/5/6 it is AMD's pipeline-utilisation model, the one behind
`perf stat -M PipelineL1,PipelineL2`, as a share of dispatch slots. On
Intel Granite Rapids it is Intel's TMA, as `perf stat -M TopdownL1,TopdownL2`,
as a share of pipeline slots, with level 2 under Intel's names (fetch
latency, branch mispredicts, memory/core bound, heavy operations); see
[hardware support](hardware.md#granite-rapids). The table below uses
the AMD names:

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

Node-level counters, per node and per socket (AMD Zen 4/5/6, Intel
Granite Rapids without the remote share; any level,
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

## Fingerprint

A compact characterisation of the run, worked out from the snapshot after
the run (it costs nothing while the application runs). The first line
splits the run into its largest parts and says what limits each:

```
 Fingerprint            compute 63% memory-latency bound; MPI 12% bandwidth (large messages); OpenMP barrier wait 12%
```

Below it, fixed axes, all percentages, so runs of different applications
or machines can be put side by side:

| Group | Axes | Needs |
|---|---|---|
| Where the time goes (% of elapsed) | MPI, rank imbalance, I/O, OpenMP serial, OpenMP barrier wait | any level |
| Compute pipeline (% of slots) | retiring, frontend bound, bad speculation, backend memory, backend core | `-l topdown` or higher |
| Use of the node (% of capability) | CPU utilisation, DRAM bandwidth, FLOP rate, packed FP ops | DRAM and FLOP: `-l roofline` or `full`, with a calibration |

An axis that was not measured is left out (`null` in JSON), never estimated.

How the first line is built:

- **Rank imbalance** is the time the average rank waits inside MPI for
  the slowest one, so it is taken out of the MPI share: "MPI 12%" is MPI
  time that better balance would not remove.
- **MPI** is described as *collectives* when they take 60% or more of MPI
  time; otherwise as *latency (small messages)* or *bandwidth (large
  messages)*, depending on whether half the point-to-point time is in
  messages below 16 KiB.
- **OpenMP serial** and **barrier wait** are taken out of compute.
- **Compute** is named from the strongest evidence available, in this
  order: *memory-bandwidth bound* (at least 60% of calibrated DRAM
  bandwidth); *FLOP bound* (at least 50% of calibrated peak); the largest
  top-down stall (*memory-latency bound* when backend memory leads with
  under 40% of DRAM bandwidth, *memory bound* when bandwidth was not
  measured, *core bound*, *frontend bound*, *bad-speculation bound*);
  *retiring well* (retiring at least 50%); the side of the roofline;
  otherwise *not characterised*, with the reason.

The descriptors under the axes (IPC, GFLOP/s and DRAM GB/s per node,
FLOP per DRAM byte, MPI MB/s per rank, typical message size, memory per
rank, power) are absolute values for context. To compare several runs,
see [miniprof-fingerprint](comparing-runs.md#miniprof-fingerprint).

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
| `metrics.timeline` | time series, when requested: `t_s`, `mpi_pct`/`io_pct`/`ipc` (min/avg/max arrays), `gflops`, `heatmap`, `memory` per node |
| `overhead` | miniprof's own cost, by phase |

Every block has `available`, and a `reason` when it is false.
`miniprof-csv` (or `miniprof-report --csv DIR`) flattens the tables into
CSV files for spreadsheets and plotting.
