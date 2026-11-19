# Internals: how miniprof works, and how to extend it

This is the guide for developers who want to change miniprof or add to it.
It explains how the pieces fit together, what each source file is
responsible for, the rules the hot paths must follow, and, step by step,
how to add the things people most often need: a new CPU, a counter, an MPI
function, a metric block, a report section, an opt-in feature.

Read [development](development.md) first for building and running the
tests; this page assumes you can do both.

- [1. The mental model](#1-the-mental-model)
- [2. Repository map](#2-repository-map)
- [3. Life of a snapshot run](#3-life-of-a-snapshot-run)
- [4. Rules for code that runs inside the application](#4-rules-for-code-that-runs-inside-the-application)
- [5. The subsystems](#5-the-subsystems)
  - [5.1 Interception: MPI and I/O wrappers](#51-interception-mpi-and-io-wrappers)
  - [5.2 The counter engine](#52-the-counter-engine)
  - [5.3 Snapshot: per-rank records, reductions, the JSON](#53-snapshot-per-rank-records-reductions-the-json)
  - [5.4 OpenMP (OMPT)](#54-openmp-ompt)
  - [5.5 Node memory: the helper](#55-node-memory-the-helper)
  - [5.6 Calibration and the roofline](#56-calibration-and-the-roofline)
  - [5.7 Hotspots and the symbolizer](#57-hotspots-and-the-symbolizer)
  - [5.8 Timeline](#58-timeline)
  - [5.9 CPU identification](#59-cpu-identification)
  - [5.10 Sample and trace modes](#510-sample-and-trace-modes)
- [6. The JSON contract](#6-the-json-contract)
- [7. The Python tools](#7-the-python-tools)
- [8. Launcher, installation, module files](#8-launcher-installation-module-files)
- [9. How testing works](#9-how-testing-works)
- [10. Recipes](#10-recipes)
- [11. Pitfalls already paid for](#11-pitfalls-already-paid-for)

## 1. The mental model

miniprof is **one shared library, `libminiprof.so`, that the application
loads without being rebuilt**. The launcher (`bin/miniprof`) puts it in
`LD_PRELOAD` for the application process only. Because the library defines
`MPI_Init`, `MPI_Send`, `read`, `pthread_create` and so on, the dynamic
linker sends the application's calls to miniprof first; miniprof does its
bookkeeping and calls the real function (`PMPI_*` for MPI, `dlsym(RTLD_NEXT)`
for libc).

Four modes share that library, chosen by `MINIPROF_MODE`:

| Mode | What it does | Main files |
|---|---|---|
| `snapshot` (the launcher's default) | running totals per rank, reduced at `MPI_Finalize` into one JSON document; reports are made from the JSON afterwards | `snapshot.c` and everything it calls |
| `sample` | `SIGPROF` sampling of call stacks, a CrayPat-style text profile | `sampler.c`, `profile_store.c`, `aggregator.c` |
| `trace` | exact entry/exit of chosen functions (`-finstrument-functions`), per-call MPI | `instrument.c`, `trace_store.c` |
| `off` | loaded, does nothing (overhead baseline) | |

Almost all recent work is in **snapshot** mode, and so is most of this
guide. Its design principle is: **record cheaply while the application
runs, decide and compute everything at the end.** No per-call records are
kept; every wrapper adds to a counter. Everything expensive (naming
functions, reductions across ranks, deriving metrics, writing reports)
happens at `MPI_Finalize` or later, in Python.

The second principle is: **never guess.** A metric that was not measured is
`null` in the JSON with a `reason`, and the reports say why. A metric that
is a bound (DRAM fills, the estimate roofline) is labelled as one.

## 2. Repository map

```
include/miniprof.h      public API (regions: miniprof_region_begin/end), version, limits
src/                    the library, the helper and the calibration tool (C)
tools/                  reports and validation (Python 3, standard library only; shell)
packaging/              launcher script, tool wrapper template, module file templates
examples/               test and demo programs (MPI, C and Fortran)
tests/                  unit tests and report checks (C and Python), fixtures
docs/                   user and developer documentation
```

Source files by role (sizes give a sense of where the weight is):

| Layer | Files | Role |
|---|---|---|
| entry | `miniprof.c` (~400 lines), `miniprof_fortran.c` | `MPI_Init`/`MPI_Finalize` interception, mode switch, Fortran init/finalize bindings |
| interception | `mpi_trace.c` (~830), `io_trace.c` (~690) | MPI (C and Fortran) and POSIX/stdio/MPI-IO wrappers |
| snapshot core | `snapshot.c` (~2600) | init order, per-rank record, reductions and gathers, every JSON block |
| measurement | `counters.c` (~1100), `ompt.c` (~540), `hotspots.c`, `timeline.c`, `nodemon_client.c` | core counters, OpenMP, CPU-time sampling, time series, the memory-helper client |
| separate programs | `nodemon.c` (`miniprof-nodemon`), `calibrate.c` (`miniprof-calibrate`), `mpa_check.c` | privileged memory helper, node ceilings, `.mpa` validator |
| support | `sysinfo.c`, `symbolizer.c`, `json_writer.c`, `spawn.c` | CPU/node/rank metadata, PC → name, streaming JSON, `popen` without `LD_PRELOAD` |
| sample/trace modes | `sampler*.c`, `profile_store.c`, `path_store.c`, `aggregator.c`, `mpa_writer.c`, `instrument.c`, `trace_store.c`, `trace_aggregator.c` | the older two-pass sampling and tracing workflow |

## 3. Life of a snapshot run

```mermaid
sequenceDiagram
    participant L as bin/miniprof (every rank)
    participant A as application
    participant M as libminiprof.so
    participant H as miniprof-nodemon (one per node)
    L->>A: exec with LD_PRELOAD (rank 0: as a child, waits)
    A->>M: MPI_Init
    M->>M: PMPI_Init, then snapshot_init()
    M->>H: node leader spawns the helper
    loop the application runs
        A->>M: MPI_*, read/write, pthread_create, OMPT callbacks
        M->>M: add to counters (atomics, rdpmc)
        H->>H: sample memory controllers every 100 ms
    end
    A->>M: MPI_Finalize
    M->>H: "stop"; read totals and series
    M->>M: snapshot_finalize(): collect, reduce, gather to rank 0
    M->>M: rank 0 writes snapshot.json
    M->>A: PMPI_Finalize
    L->>L: rank 0: miniprof_report.py → report.txt/html, csv/
```

### 3.1 The launcher

`packaging/miniprof` (bash) runs on every rank, after `mpirun`/`srun`. It
parses its options, works out the rank from the launcher's environment
(`OMPI_COMM_WORLD_RANK`, `PMIX_RANK`, `PMI_RANK`, `SLURM_PROCID`), checks
the application can be profiled (dynamically linked, same MPI soname as
the library, recorded at install time), exports `MINIPROF_*` variables and
starts the application with `LD_PRELOAD`. Non-zero ranks `exec` the
application. Rank 0 runs it as a child (stdin passed through, signals
forwarded) so that afterwards it can run `miniprof_report.py` on the
snapshot and print the summary. Options map to environment variables, so
anything the launcher does can also be done by hand:

| Launcher | Environment |
|---|---|
| (always) | `MINIPROF_MODE=snapshot`, `MINIPROF_SNAPSHOT_OUTPUT=<dir>/snapshot.json` |
| `-l L` | `MINIPROF_SNAPSHOT=L` |
| `-H` | `MINIPROF_HOTSPOTS=1` |
| `-t` | `MINIPROF_TIMELINE=1` |
| `-s` | `MINIPROF_MODE=sample`, `MINIPROF_OUTPUT`, `MINIPROF_MPA_OUTPUT` |

### 3.2 Init: `MPI_Init` → `miniprof_init` → `snapshot_init`

`MPI_Init`/`MPI_Init_thread` (`miniprof.c`; Fortran `mpi_init_` in
`miniprof_fortran.c`) call `PMPI_Init` first, then `miniprof_init`, which
reads `MINIPROF_MODE` and calls `snapshot_init`. Its order matters:

1. level from `MINIPROF_SNAPSHOT`, output path, start time, thread level;
2. `sysinfo_collect_node/rank`: CPUID, NUMA, SMT, affinity, paranoid;
3. `cnt_init(uarch, group, rank)`: open the counter group on this thread
   (see 5.2);
4. node leader (rank 0 of `MPI_COMM_TYPE_SHARED`) starts the memory helper
   with `nodemon_start`; it does not wait for the helper's answer;
5. `ompt_snapshot_begin()`, `hotspots_start()` (no-op unless requested);
6. switch the wrappers on: `mpi_trace_set_mode(MPI_TRACE_SNAPSHOT)`,
   `io_trace_set_enabled(1)`, rusage baseline;
7. `tl_init()` and the timeline's first counter mark (no-op unless
   requested).

The time spent here is reported as `overhead.init_s`, phase by phase, and
the application's clock (`elapsed_s`) starts after it.

### 3.3 While the application runs

Nothing is written, sent or allocated per event. The work is:

- every wrapped MPI call: two clock reads, a counter read before and after
  (rdpmc, tens of ns) to split counts into compute and MPI, a few relaxed
  atomic adds (calls, bytes, ns, size histogram);
- every wrapped I/O call: clock reads and atomic adds;
- OMPT callbacks: thread-local stacks, atomic adds;
- new threads: `pthread_create` is interposed so the new thread opens its
  counter group on its first instruction;
- user regions (`miniprof_region_begin/end`): a counter read each;
- opt-in: the hotspots `SIGPROF` handler, the timeline's bucket adds;
- the helper samples the node's memory controllers in its own process.

### 3.4 Finalize: `snapshot_finalize`

Collective, on every rank, in this order (each phase timed into
`overhead.finalize_phase_s`):

1. stop the hotspot timer and the timeline; the node leader stops the
   helper and reads its totals (`nodemon_stop`);
2. `collect_parallel`: freeze MPI/I/O accumulators, OMPT, counters
   (`cnt_collect`), rusage into this rank's `rank_rec_t`;
3. node topology (`MPI_COMM_TYPE_SHARED` split, leader communicator);
4. reductions that only rank 0 needs: message-size histograms
   (`MPI_Reduce`), user regions (`reduce_regions`), OpenMP regions
   (`reduce_omp_regions`);
5. hotspots: an `Allreduce` decides whether anyone sampled; if so each rank
   names its functions and rank 0 gathers the top 48 per rank;
6. timeline (`reduce_timeline`): common bucket width, per-bucket
   min/sum/max reductions, heatmap;
7. `MPI_Gather` of every `rank_rec_t` (fixed size, as bytes) and, from node
   leaders, every `node_rec_t` (including the helper's results);
8. rank 0: `write_json` writes `snapshot.json`.

Then `PMPI_Finalize`, and the launcher on rank 0 runs the reports.

## 4. Rules for code that runs inside the application

The library runs inside someone else's job, often on thousands of ranks.
These rules are what keeps it safe; every hot path in the code follows them.

1. **Off means one branch.** A feature that is not enabled must cost a
   predictable branch and nothing else (`if (!g_mode ...) return CALL;`,
   `if (tl_on) ...`). Opt-in features (`--hotspots`, `--timeline`) exist
   because their cost is measurable; the user decides.
2. **No allocation, locks or stdio on hot paths.** Use fixed-size arrays
   and relaxed atomics (`__atomic_fetch_add(..., __ATOMIC_RELAXED)`).
   A mutex is acceptable only on a rare path (first sighting of a region,
   doubling the timeline width).
3. **Signal handlers are async-signal-safe.** The hotspots and sample-mode
   handlers record a PC into a lock-free table and return. All naming
   happens at finalize.
4. **Never recurse into yourself.** `g_miniprof_tool_depth` (thread-local)
   is raised inside every recorded MPI and MPI-IO call; wrappers that see it
   raised pass straight through. That is what stops a Fortran binding that
   calls the C binding from being counted twice, and keeps the MPI
   library's own POSIX I/O out of the application's I/O.
5. **Collective decisions are collective.** Any optional step at finalize
   that involves communication must be decided by an `Allreduce` of the
   local decision, never by each rank reading its own environment (a
   variable may not reach every rank: Open MPI forwards only `-x`
   variables). A gather that only some ranks join hangs the job.
6. **Fixed-size records, sent as bytes.** Per-rank and per-node records
   (`rank_rec_t`, `node_rec_t`) are plain structs gathered with `MPI_BYTE`.
   Every rank runs the same binary, so layout is identical. Keep them free
   of pointers. They are per rank, so think about the size: 10 000 ranks ×
   1 KB is 10 MB at rank 0. Large per-rank arrays should be **reduced**,
   not gathered (the timeline does this).
7. **Helper processes without `LD_PRELOAD`.** Use `mp_popen` (`spawn.c`)
   or `posix_spawn` with a cleaned environment. A child that inherits
   `LD_PRELOAD` loads libminiprof and the MPI library into every `sh` and
   `addr2line` (seconds on a cold network filesystem), and `fork()` is
   unsafe in a process with registered memory.
8. **Report the cost.** Anything that takes time at init or finalize gets
   a phase in the `overhead` block.

## 5. The subsystems

### 5.1 Interception: MPI and I/O wrappers

**MPI** (`mpi_trace.c`, `mpi_trace.h`). The list of wrapped functions is
one X-macro table, `MPI_TRACE_FUNCS(X)` in `mpi_trace.h`: each entry is
`X(ID, "MPI_Name", CLASS)` with class `P2P`, `COLLECTIVE`, `WAIT` or `RMA`.
From it come the `MPIF_*` ids, the name and class tables, and the fixed
arrays of `mpi_snap_t` (calls, bytes, ns per function; size histograms per
class). Each C wrapper is one line:

```c
int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype dt, MPI_Op op, MPI_Comm comm)
{ C_WRAP(ALLREDUCE, PMPI_Allreduce(sendbuf, recvbuf, count, dt, op, comm), nbytes(count, dt)); }
```

`C_WRAP(ID, CALL, BYTES)` returns `CALL` directly when no mode is on or the
tool depth is raised; otherwise it raises the depth, marks the counter
boundary (`cnt_mpi_enter`), times `CALL`, marks the exit (`cnt_mpi_exit`),
lowers the depth and calls `record(id, dt, bytes)`. `BYTES` is evaluated
after the call, so `PMPI_Type_size` is not timed as MPI. The byte rules
(never read an argument `MPI_IN_PLACE` makes undefined) are documented
above each wrapper.

Fortran wrappers (`F_WRAP`) time the call and forward the arguments
**unchanged** to the library's own Fortran binding (`pmpi_allreduce_`).
Never convert to C and call the C API: Fortran sentinels like
`MPI_IN_PLACE` are library-private addresses (see section 11).

**I/O** (`io_trace.c`). Defines `open`, `read`, `write`, `pread`, the
`_FORTIFY_SOURCE` variants (`__read_chk`...), stdio (`fopen`, `fread`,
`fwrite`, `getline`...) and MPI-IO (`MPI_File_*`). Real functions are
resolved eagerly in a constructor with `dlsym(RTLD_NEXT)`. A bitmap of
file descriptors tracks which files the application opened after init, so
sockets, pipes and the MPI library's files are left out. Times are added
with `ADD_T`, which also feeds the timeline.

### 5.2 The counter engine

`counters.c` / `counters.h`: per-thread hardware counter groups through
`perf_event_open`, read at MPI boundaries, regions and finalize.

**Kinds and tables.** `cnt_kind_t` lists *semantic* counters (`CK_CYCLES`,
`CK_FLOPS`, `CK_DRAM_FILLS`, `CK_SLOTS`, ...); their JSON names are in
`k_kind_names` (a `_Static_assert` keeps the two in step). `k_groups[]`
maps `(uarch, group)` to a list of events:

```c
{ "zen5", "base", 3, 6, {            /* uarch, group, required, total */
    { CK_CYCLES, "ls_not_halted_cyc", RAW(0x076, 0x00) },
    ...
} },
```

Groups are `base` (level `basic`), `roofline`, `vector` (level `full`)
and the top-down sets (`td_l1`, `td_febe`, `td_rbs` on Zen; one
`td_metrics` on Intel). `find_group` looks a pair up (`table_uarch` maps a
CPU to another's tables, e.g. Zen 6 → Zen 5); `cnt_lookup` falls back to
the `generic` group (cycles and instructions).

Each event has `type`, `config`, `config1` (Intel offcore-response MSR),
and `weight`. Encoding macros: `CNT_AMD_RAW(event, umask)` (AMD's split
event select) and `CNT_INTEL_RAW(event, umask)`.

**Fitting a group.** A CPU has few counters, and the NMI watchdog may hold
one. Events `[0, required)` must fit; the rest are optional, ordered by
value. `fit_set` opens the full group on the calling thread, spins 2 ms and
checks `time_running > 0` (a group that opens but is never scheduled reads
zeros), then trims optional events from the end until it fits. The JSON
lists which events were active.

**Logical events.** Some quantities need several events: Intel FLOPs are
five `FP_ARITH_INST_RETIRED` events weighted 1, 2, 4, 8, 16. Events with
`weight > 0` and the same kind fold into **one logical event**.
`build_logical` builds, per set, a logical group (`g_lsets`) and a map from
physical to logical index with weights; counting stays physical, and
`to_logical` folds counts at collect and region read-out. Everything
outside `counters.c` sees one `flops` event, on every CPU.
`tests/test_counters_fold.c` checks this with software events.

**Threads.** Each thread has a slot (`thread_slot_t`): its fds, mmapped
pages for rdpmc, the last reading, and accumulators per bucket (compute,
MPI, OpenMP idle). Threads get a slot when:

- they call `cnt_init` (the MPI_Init thread);
- they are created after init: `pthread_create` is interposed and the
  trampoline calls `thread_open()` before the user's function;
- OMPT reports a thread (`cnt_thread_hook`); OpenMP pools created before
  `MPI_Init` are remembered and attached by thread id in `attach_pending`;
- any other thread reaches a boundary (lazy `thread_open`).

A thread that exits gives its slot back: `thread_open` registers the slot
with a `pthread_key` whose destructor, `retire_slot`, runs on the exiting
thread, adds its final counts to per-rank totals for exited threads
(`g_ret`, which `cnt_collect` includes), closes its descriptors, unmaps its
pages and puts the slot on a free list. Without this, an application that
creates short-lived threads would run out of file descriptors.
`g_slot_lock` orders retirement, slot reuse and `cnt_collect`; none of
them is on a hot path. `make run-threads-validate` checks it under a
256-descriptor limit.

**Reading.** `read_now` uses `rdpmc` under the mmap page's seqlock when the
kernel allows it and every event of the group is on the PMU; otherwise one
`read()` of the group (`PERF_FORMAT_GROUP`). A group can opt out of rdpmc
(`no_rdpmc`): Intel's top-down metric events cannot be read as counts with
rdpmc. `switch_bucket` adds the delta since the last reading to the current
bucket and switches: `cnt_mpi_enter/exit` from the MPI wrappers (unless
`MINIPROF_COUNTERS_SPLIT=0`), `cnt_omp_worker` from OMPT (worker threads'
spin between regions goes to the idle bucket, so it does not dilute IPC).

**Top-down sets.** On Zen the 12 pipeline events do not fit in one group,
so they are split into three sets; each thread counts one set for the
whole run, `set = (rank + slot) % sets`, and ratios are only formed within
a set (`td_compute` in `snapshot.c`). Intel's TMA uses the slots counter
and `PERF_METRICS`, so one `td_metrics` group holds levels 1 and 2.

**Collect.** `cnt_collect` reads every slot's final values with `read()`
(works for exited threads), folds to logical events and fills a
`cnt_result_t`: totals, compute, MPI, idle, per set, plus read statistics
and multiplexing.

**Test paths.** `MINIPROF_COUNTERS_TEST=1` selects `swtest` tables of
software events (task-clock, page faults) so CI exercises the engine
without a PMU; `=tma` selects `swtest_tma`, the Intel one-group top-down
path. Their values mean nothing; only plumbing is checked.

### 5.3 Snapshot: per-rank records, reductions, the JSON

`snapshot.c` is long but regular. Its parts, in file order:

- **state**: init timings, `g_node`/`g_rank` (sysinfo), counter result
  (`g_cnt`, `g_cnt_group`), helper result (`g_nm`), reduced regions,
  OpenMP regions, hotspots and timeline buffers (rank 0);
- **`rank_rec_t`**: everything one rank contributes, fixed size;
  **`node_rec_t`**: per node, including `nm_result_t`;
- **`snapshot_init`** (3.2) and the JSON helpers;
- **`write_*` functions**, one per JSON block: `write_node`, `write_rank`,
  `write_parallel` (time split, MPI, I/O, CPU), `write_counters`,
  `write_topdown` (with `td_compute`, `write_td`), calibration
  (`cal_load`, `write_calibration_ref`), `write_memory`, `write_roofline`,
  `write_openmp`, `write_hotspots`, the timeline (`reduce_timeline`,
  `write_timeline`);
- **`collect_parallel`**, the reductions, **`snapshot_finalize`** (3.4).

Each `write_*` reads the gathered records and derives what it writes; the
derivations (IPC, GB/s, imbalance, percentages) live here, in C, so the
JSON is self-contained and every report agrees. A block that could not be
measured writes `"available": false` and a `reason`.

`metrics.collected` lists the blocks a run actually has; consumers use it
instead of inferring from nulls. `MINIPROF_SNAPSHOT_SCHEMA_VERSION` in
`snapshot.h` is bumped when blocks or keys are added (minor) or changed
incompatibly (major); `tools/snapshot_check.py` accepts the supported
major version.

### 5.4 OpenMP (OMPT)

`ompt.c` is an OMPT tool. The runtime finds it through the exported
`ompt_start_tool`, which returns NULL unless the mode is snapshot. Callbacks
(`on_parallel_begin/end`, `on_implicit_task`, `on_sync_region_wait`,
`on_mutex_acquire/acquired`, `on_task_schedule`, `on_thread_begin`) record
on the calling thread's clock into thread-local stacks and atomics: time in
parallel regions (so serial time is the rest), per-thread work and barrier
wait, lock waits, per-region imbalance keyed by the construct's code address
(`<object>+0x<offset>`, so it matches across ranks despite ASLR). It also
tells the counter engine when a worker starts and stops working.
`ompt_abi.h` holds the few OMPT types used, so no `omp-tools.h` is needed to
build. GCC's libgomp has no OMPT: the block is then unavailable.

### 5.5 Node memory: the helper

Memory controllers and RAPL are node-wide PMUs that perf only opens with
`CAP_PERFMON` (or `perf_event_paranoid <= 0`). The application stays
unprivileged: the node leader spawns **`miniprof-nodemon`**, the only
binary given the capability.

`nodemon.c` is deliberately closed. It takes only `--uarch` and
`--interval-ms`, opens a fixed set of events chosen by uarch in `setup()`
(`setup_df_dram`, `setup_intel_imc`, Zen 5's UMC loop, `setup_energy`),
**drops all capabilities** (`drop_caps`), then samples every interval and
answers over a pipe with a line protocol documented in `nodemon.h`:

```
ok sockets=2 chans=12 interval_ms=100 events=98
... "stop" from the client ...
result elapsed_ns=... samples=... multiplexed=0
socket 0 rd=<B> wr=<B> remote_in=<B> energy_uj=... peak_rd=... peak_wr=...
chan 0 3 rd=<B> wr=<B>
remote unmeasured                 (no remote-socket events on this CPU)
series t0_ns=... width_ns=... n=...
sv <rd B> <wr B> <energy uJ>      (the timeline, n lines)
peak node=<B/s>
end
```

`nodemon_client.c` spawns it without `LD_PRELOAD`, does not wait for `ok`
(that would delay every rank's first collective), and parses the answer
into `nm_result_t` at stop. `MINIPROF_NODEMON_TEST=1` runs the helper on
software events (no privilege) for CI.

### 5.6 Calibration and the roofline

`calibrate.c` (`miniprof-calibrate`) measures a node once per node type:
DRAM bandwidth (read and triad, per socket and node, one pinned thread per
physical core) and peak FLOP rate per width and precision (independent FMA
chains in registers, per-function `target` attributes so one binary runs
on any x86-64). It writes `calibration_<host>.json`; `cal_load` in
`snapshot.c` picks the file for the node (by hostname, else CPU brand) from
`MINIPROF_CALIBRATION`.

`write_roofline` combines FLOPs (core counters, this job's threads) with
DRAM bytes (helper, whole node) per node; the DRAM ceiling depends on the
run's read share (`cal_bw_mix`). Without whole-node bytes it falls back to
core-counter DRAM reads and marks the result an `estimate`. Per-region
points need complete DRAM reads per region from core counters (Zen's fills
+ L2-prefetch misses at level `roofline`); they are not offered where the
core cannot see all reads (Granite Rapids).

### 5.7 Hotspots and the symbolizer

`hotspots.c`: with `MINIPROF_HOTSPOTS=1`, `ITIMER_PROF` sends `SIGPROF`
every N ms of process CPU time; the handler records the interrupted PC
(from `ucontext`) into the lock-free `profile_store`. At finalize,
`hotspots_collect` names PCs with `symbolizer_quick_name` (in-process ELF
symbol tables, a binary-search index, a DSO cache) and, for binaries with
`.debug_line`, `symbolizer_inline_names` (one `addr2line -f -C` per DSO,
addresses on stdin) for the PCs of the top 64 functions, so inlined kernels
get their own names. `symbolizer.c` also holds the sample mode's full
resolver (dladdr, addr2line, nm fallback) and group classification
(USER, MPI, OMP, LIBC, ...).

### 5.8 Timeline

`timeline.c` / `timeline.h`, opt-in. Per rank, buckets of
`MINIPROF_TIMELINE_INTERVAL_MS` hold MPI and I/O nanoseconds (`tl_add`,
called from the wrappers; a call spanning buckets is split) and the
first counter mark of the MPI thread in each bucket (`tl_mark`, from
`switch_bucket` and `cnt_collect`). When the run outgrows `TL_BUCKETS`
(512), neighbours merge and the width doubles (`grow_locked`, under a
mutex). At finalize, `reduce_timeline` in `snapshot.c`:

1. `Allreduce` MAX of the width; each rank rebins to it (`tl_rebin`);
2. per bucket, the rank's MPI and I/O share, IPC and FLOP rate (counts
   interpolated linearly between marks, `tl_cum`);
3. `MPI_Reduce` MIN/MAX/SUM per bucket, and a heatmap of MPI share by rank
   group (≤ 64 rows) reduced with SUM.

The node series comes from the helper's `series` lines. The helper
spreads each sample over the buckets it overlaps (filing it whole where it
ends aliases with the poll period: an empty bucket, then a doubled one) and
scales its partial last bucket to a full one. The node leader dates the
series against its own timeline start (`series_offset_s`), and
`write_timeline` resamples it onto the ranks' buckets, so every series in
the JSON shares `t_s`.

### 5.9 CPU identification

`sysinfo_classify_uarch(vendor, family, model)` in `sysinfo.c` turns CPUID
into a `uarch` string (`zen4`, `zen5`, `zen6`, `granite_rapids`, ...) and a
name. **The uarch string is the key for everything CPU-specific**: counter
tables (`counters.c`), dispatch width (`cnt_dispatch_width`), the helper's
`setup()` and the client's allow-list (`nodemon_client.c`). The raw family
and model are kept in the JSON too.

### 5.10 Sample and trace modes

The older workflow, still supported: **sample** (`MINIPROF_MODE=sample`)
records leaf PCs and call paths from `SIGPROF` (`sampler.c`,
`profile_store.c`, `path_store.c`), symbolizes at finalize, gathers per
(function, file, line) and writes `miniprof_report.txt` (`aggregator.c`)
and an `.mpa` list of functions worth tracing (`mpa_writer.c`). **trace**
needs a build with `-finstrument-functions`; `instrument.c` filters the
`__cyg_profile_func_enter/exit` hooks against the `.mpa` list (resolved
with `nm`, because static functions are not in `.dynsym`), `trace_store.c`
keeps inclusive/exclusive time and counters per call, and
`trace_aggregator.c` writes the report. See
[sampling and tracing](sampling-and-tracing.md).

## 6. The JSON contract

`snapshot.json` is the interface between the C library and everything
else. Conventions every block follows:

- `"available": true|false`, and `"reason"` when false. Never omit a block
  silently; write `null` for a block the level does not collect.
- A `"definitions"` object in each block says what each key means, in the
  words a report would use. Reports quote it rather than re-deriving.
- Units are in key names: `_s`, `_ns`, `_pct` (0–100), `_gb_s`, `_bytes`,
  `_w`, `_j`. Rates are per second unless the key says otherwise.
- Statistics over ranks are `{"min", "avg", "max"}` objects.
- `NaN` and infinities are written as `null` (`jw_dbl`).
- Strings are escaped by `json_writer.c`; the HTML report escapes again
  (`tests/fixtures/report_escape.json` tries to break it).

Adding keys is backwards compatible; the Python tools read with
`.get(...)` and treat missing keys as "not measured", so old snapshots
keep working with new tools.

## 7. The Python tools

All standard library, Python 3.6+, runnable from the source tree or as
installed wrappers (`miniprof-report`, `-diff`, `-csv`, `-fingerprint`,
generated from `packaging/miniprof-tool.in`).

| File | Entry points | Notes |
|---|---|---|
| `miniprof_report.py` | `report(doc, args)` → text; `observations(doc, args)`; `oneline(doc)`; `main()` | `_sections(doc)` extracts what the rules read; `THRESHOLDS` is the single table of rule thresholds (defaults, INI file via `apply_thresholds`, command line); text helpers `bar`, `spark`, `timeline_lines`, `human_bytes`, `human_time` |
| `miniprof_html.py` | `render(doc, obs, thresholds)` | inside it, `section(key, title, body)` adds a section and its nav link; chart helpers: `hstack`, `ranked_bars`, `columns`, `rank_chart`, `roofline_chart`, `time_chart`, `heatmap`; CSS variables for colours (`--s1..--s4`), light and dark |
| `miniprof_csv.py` | `write_csv(doc, outdir)` | one CSV per table; `flat()` flattens nested dicts, `write_table()` writes one file |
| `miniprof_diff.py` | `analyse(A, B)` → dict; `verdict`; `text`; `html` | everything compared lives in `analyse`; text and HTML only format it |
| `miniprof_fingerprint.py` | `fingerprint(doc)`; `text_one`, `text_many`, `html_section` | classification constants at the top |

Conventions: functions take the parsed JSON and return strings or plain
data; every value read may be missing or `None`; the HTML is one
self-contained file (inline CSS and JS, no network), with hover text in
`data-tip` attributes; text reports are 78 columns.

## 8. Launcher, installation, module files

`make install PREFIX=... MODULE_VERSION=...` installs:

```
bin/miniprof                     the launcher (packaging/miniprof)
bin/miniprof-{report,diff,csv,fingerprint}   wrappers around share/miniprof/tools/*.py
bin/miniprof-calibrate
lib/libminiprof.so, lib/miniprof-nodemon
share/miniprof/tools/            Python tools, nodemon_ready.sh
share/miniprof/VERSION, mpi_soname   (the launcher's MPI check)
share/modulefiles/miniprof/<v>.lua, share/modulefiles-tcl/miniprof/<v>
include/miniprof.h, share/doc/miniprof/
```

The launcher's `--help` is its own header comment (lines up to the first
non-comment), so documenting an option there documents it in `--help`.
Module files set `PATH`, `MINIPROF_HOME` and, if present,
`MINIPROF_CALIBRATION`; they never set `LD_PRELOAD`.

## 9. How testing works

Three layers, all driven by `make check` (what CI runs on a hosted runner
with no PMU and no privileges):

1. **Unit tests**, no MPI: `tests/test_units.c` (sysinfo, JSON writer),
   `tests/test_counters_fold.c` (logical events), the OMPT ABI test.
2. **Known-answer programs + validators.** Each feature has an example
   whose answer is known by construction, and a Python validator that
   checks the snapshot against it: `counters_validate` (exact FLOPs per
   region), `imbalance_validate`, `omp_validate`, `topdown_validate`
   (one region per top-down category), `membw_validate` (known DRAM
   bytes), `timeline_validate` (three known phases), `diffusion3d_hot`
   (a larger `diffusion3d`; enough samples, inlined kernels named). On a machine without the needed hardware the
   validator exits 77 and the target prints SKIP.
3. **Software stand-ins**: `MINIPROF_COUNTERS_TEST`, `MINIPROF_NODEMON_TEST`
   make the counter and memory plumbing run anywhere, so CI covers the code
   paths even where it cannot cover the numbers.

Report checks (`tests/check_report_html.py`, `check_diff.py`,
`check_fingerprint.py`) run on fixtures and on every
`miniprof_snapshot_*.json` the run produced: well-formed HTML, escaping,
expected sections.

**Real hardware** is validated by the scripts in `tools/`
(`validate_counters.sh`, `validate_topdown.sh`, `validate_memory.sh`,
`validate_roofline.sh`, `validate_intel.sh`, the `probe_*` scripts), run
by hand on a node of each type; they compare with `perf stat` and known
byte and FLOP counts. Every CPU in [hardware support](hardware.md) went
through them.

## 10. Recipes

### 10.1 Add a CPU

The Granite Rapids work (PR #55) is a complete worked example.

1. **Identify it.** Add the family/model to `sysinfo_classify_uarch` with a
   new `uarch` string. If it shares another CPU's encodings, map it in
   `table_uarch` (`counters.c`) and stop at step 5.
2. **Probe it.** Write or adapt a `tools/probe_*.sh` that runs kernels with
   known FLOP and byte counts under `perf stat` with *your raw encodings*,
   opens the groups exactly as miniprof will (user space,
   `PERF_FORMAT_GROUP`), and lists the uncore PMUs. Run it on the node.
   Take encodings from the kernel's `tools/perf/pmu-events/arch/x86/<cpu>`
   JSON; verify, do not trust.
3. **Counter tables.** Add `k_groups[]` entries for `base`, `roofline`,
   `vector` and the top-down set(s), using existing kinds where the meaning
   matches (and documenting where it differs), weights for multi-event
   quantities, required events first and optional ones by value.
   `cnt_dispatch_width` must return non-zero for top-down. New kinds go at
   the end of `cnt_kind_t` with a name in `k_kind_names`.
4. **Derived metrics.** If a kind is new, derive it in `write_counters` /
   `td_compute` and add a `definitions` entry. Keep JSON keys the same
   across vendors where the meaning is the same.
5. **Memory.** Add a `setup_*` in `nodemon.c` for the memory-controller
   PMUs (found through sysfs, `cpumask` gives one CPU per socket), call
   `setup_energy`, and add the uarch to both allow-lists (`nodemon.c`
   `main`, `nodemon_client.c`). Say in `coverage` if only part of the node
   is seen; print `remote unmeasured` if remote traffic is not counted.
6. **Validate** with the `tools/validate_*.sh` scripts on the node, fix,
   repeat. Record the result in [hardware support](hardware.md).

### 10.2 Add a counter or a derived metric

- Same group, new event: add it to the table (optional, at the right place
  in the value order), a kind if needed, and the derivation in
  `write_counters`. Check the group still fits with the NMI watchdog on.
- Derived only: compute it in `write_counters` from existing kinds and add
  a `definitions` entry. Then show it in the text report (counters
  section of `report()`), HTML (`render`, counters section), and if it is
  comparable across runs, `analyse` in the diff.

### 10.3 Wrap another MPI function

1. Add `X(ID, "MPI_Name", CLASS)` to `MPI_TRACE_FUNCS` in `mpi_trace.h`.
2. Add the C wrapper with `C_WRAP` and a byte rule that is valid on every
   rank (`MPI_IN_PLACE`!), and the Fortran wrapper with `F_WRAP`
   forwarding to `pmpi_name_`.
3. Add a call to `examples/mpi_ext_validate.c` / `.f90` and its check.

The class decides where its time appears (`by_class`); IDs are internal, so
the JSON stays compatible.

### 10.4 Add a metric block to the snapshot

1. **Measure** in a new `src/<feature>.c` with a small header: an init
   (no-op when disabled), hot-path functions that follow section 4, and a
   collect function filling a fixed-size result.
2. **Wire** it: init call in `snapshot_init` (with an `INI_*` phase and `INI_MARK` if it
   costs time; finalize phases likewise), collect in `collect_parallel` or a dedicated step in
   `snapshot_finalize`.
3. **Combine across ranks**: small per-rank values go into `rank_rec_t`
   (gathered); per-rank arrays are reduced (`MPI_Reduce`) into a rank-0
   buffer; optional steps are decided collectively.
4. **Write** `write_<feature>(jw, ...)` with `available`, `reason`,
   `definitions`; call it in `write_json` and add the name to
   `metrics.collected` when present. Bump the schema minor version.
5. **Report**: text in `report()`, HTML section in `render()`, CSV in
   `write_csv()`, the diff in `analyse()` if it is comparable, the
   fingerprint if it characterises the run.
6. **Test**: a known-answer example, a validator, a `run-<feature>-validate`
   target in `make check`, a software stand-in if it needs hardware.
7. **Document**: a section in [snapshot reports](snapshot.md), variables in
   [configuration](configuration.md), the launcher option if any.

The timeline (PR #56) followed exactly these steps.

### 10.5 Add an opt-in feature with a launcher flag

Pattern of `--hotspots` and `--timeline`: an environment variable read once
at init sets a global flag; hot paths check it first. In
`packaging/miniprof`, add the option to the header comment (that is
`--help`) and the `case` (`-x|--xxx) export MINIPROF_XXX=1; shift ;;`).
Document it in the options table and "Each option in detail" of the
[user guide](user-guide.md), and add it to `test-install`.

### 10.6 Add an observation rule or threshold

Add the threshold to `THRESHOLDS` in `miniprof_report.py` (name, default,
meaning): it then works in INI files, on the command line (`--name-with-dashes`, generated from the key)
and in `--write-thresholds`. Add the rule in `observations()`, reading
values through `_sections()` and guarding every value that may be missing.
The HTML report uses the same list. Document it in
[comparing runs](comparing-runs.md#thresholds).

### 10.7 Add a report section or chart

Text: a block in `report()` using `bar()`, `spark()` and the `human_*`
formatters; keep 78 columns. HTML: build the body with the chart helpers and `table()`, then
`section("key", "Title", body)`; colours only through the CSS variables,
2 px lines, hover text in `data-tip`, a legend for two or more series. Run
`make test-report-html`, then open the page (light and dark) and look at
it.

## 11. Pitfalls already paid for

Each of these cost a debugging session; the fixes are in the code.

- **Leaf PC in a signal handler** comes from `ucontext` (`REG_RIP`), not
  `backtrace()[0]`, which is the handler itself.
- **Fortran sentinels.** Converting Fortran arguments to C broke
  `MPI_IN_PLACE` silently (zeros instead of sums). Forward to the
  library's Fortran binding; `make run-fortran-sentinels` guards it.
- **Static functions are not in `.dynsym`**, so `dladdr` cannot name them:
  use the ELF symbol table or `nm`.
- **A counter group that opens is not necessarily scheduled**: with the
  NMI watchdog holding a counter, a full group reads zeros forever. Check
  `time_running` (`fit_set`).
- **Some events read wrongly with rdpmc** (Intel top-down metrics): mark
  the group `no_rdpmc`.
- **Core DRAM-read events can be incomplete** (Zen's L1D fills miss
  L2-prefetched lines; Intel's offcore reads miss L3-prefetched lines):
  label them lower bounds; use the memory controllers for bandwidth.
- **Finalize collectives decided per rank** hang jobs: decide with an
  `Allreduce`.
- **Children inheriting `LD_PRELOAD`** cost seconds each: `mp_popen`.
- **Compilers differ at `-O2`**: GCC 11 neither vectorises nor unrolls
  where newer compilers do. Test kernels whose speed matters are built
  like the calibration (`-O3`), and validators check invariants that hold
  either way.
- **`CC` from modules**: a module's `CC=gcc` compiled MPI examples but
  could not link them; the examples Makefile uses `mpicc` unless `CC` is
  on the command line.
- **The probe's own kernels** were optimised away (pure functions called
  once instead of R times): known-answer kernels need a side effect
  (`asm volatile`) or the answer is wrong, not the counter.
