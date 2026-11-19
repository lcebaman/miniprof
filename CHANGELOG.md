# Changelog

## Unreleased

- **Partial snapshots.** A run that ends without `MPI_Finalize` (a batch
  time limit, Ctrl-C, `MPI_Abort`, `exit()`) used to write nothing. Each
  rank now leaves a record of what it measured, written from the signal
  handler without malloc, stdio or locks, and the launcher merges the
  records into a snapshot marked PARTIAL, with the usual reports: time
  split, MPI, I/O, CPU and basic counters. An application's own SIGTERM
  handler still runs. New tool `miniprof-partial` for merging by hand.
- **Memory helper hardening.** `make install NODEMON_GROUP=<group>`
  installs `miniprof-nodemon` runnable by that group only (the memory
  bandwidth and energy counters it reads are a known side channel, see
  [installing](docs/installing.md)); the shortest interval is now 50 ms
  (`MINIPROF_MEMORY_INTERVAL_MS`, was 10); the software test modes are
  refused when the helper holds capabilities.
- **No node limit.** Memory statistics across nodes are streamed; the
  4,096-node limit is gone.
- **Signal safety in sample mode.** `backtrace()` is warmed up before the
  first SIGPROF, and the handlers preserve `errno`.
- **Internal MPI calls use `PMPI_`**, so miniprof's own gathers no longer
  pass through its wrappers or another tool's.
- **CI** runs the whole suite under AddressSanitizer and UBSan
  (`make check-sanitizers`).
- **`topdown_validate`** caps its arrays at a quarter of the node's
  available memory. On a VM that reports the host's whole L3 (Granite
  Rapids) it asked for ~8 GB per rank and could be OOM-killed.

## 1.1.0 — 2026-11-19

The first tagged release. Earlier versions had only the sample and trace
modes; 1.1.0 adds **snapshot mode**, now the launcher's default: one run,
no rebuild, and a JSON snapshot with text, HTML and CSV reports. Pull
request numbers refer to https://github.com/lcebaman/miniprof.

### Snapshot mode

- Parallel runtime: time split (compute, MPI, I/O), rank imbalance, MPI by
  function and class with message-size histograms, POSIX and MPI-IO, CPU
  utilisation and binding (#4, #5, #36, #40, #41).
- OpenMP through OMPT: parallel and serial time, barrier and lock waits,
  per-region imbalance, worker spin kept out of compute (#9, #10, #38).
- Hardware counters, levels `basic` to `full`: IPC, FLOPs by vector width,
  user regions, MPI-time split, threads followed as they are created
  (#6, #7, #8).
- Top-down: AMD pipeline utilisation level 1 and 2, Intel TMA (#12, #13).
- Node memory bandwidth, NUMA traffic and energy through a small
  privileged helper (`miniprof-nodemon`), without privilege where
  `perf_event_paranoid <= 0` (#14–#17, #21).
- Calibration (`miniprof-calibrate`) and the roofline: per node, per region
  and as an estimate where node memory is partial (#18–#20, #25, #26).
- Hotspots: opt-in top functions, inlined functions named from the debug
  information (#43, #49, #51).
- Timeline: opt-in per-rank time series of MPI, I/O, IPC, FLOP rate, DRAM
  bandwidth and power (#56, #58, #59).
- Workload fingerprint (#52, #53).

### CPUs

AMD Zen 4, Zen 5 and Zen 6 (Venice; node memory on NUMA node 0 with the
RHEL 9 kernel), Intel Granite Rapids (#22–#24, #55). Other x86-64 CPUs get
IPC. See docs/hardware.md.

### MPI coverage

- C, Fortran `mpif.h` / `mpi` module, and the Fortran 2008 `mpi_f08`
  module, whose programs were not profiled at all before (#5, #64).
- 111 MPI functions: point-to-point, persistent requests, matched probe,
  completion, blocking, nonblocking and neighbourhood collectives,
  communicators and topologies (new class `comm`), one-sided including
  passive target and request-based RMA; and MPI-IO reads, writes and
  metadata (#3, #30, #63).

### Tools

- `miniprof` launcher with `--level`, `--hotspots`, `--timeline`,
  `--sample`; `make install` with Lmod and Tcl module files (#39, #42,
  #54).
- Reports: text, HTML, CSV, threshold-driven observations with a
  thresholds file (#25, #27, #37).
- `miniprof-diff` explains the difference between two snapshots;
  `miniprof-fingerprint` compares workloads (#28, #29, #52).
- The tools run on Python 3.6 or later, standard library only (#61).

### Fixes from the 1.1 audit

- Threads that exit release their counter file descriptors; before, an
  application creating short-lived threads could run out of descriptors
  (#62).
- `libminiprof.so` exports only its wrappers and public API: internal
  helpers can no longer replace an application's functions of the same
  name.
- Snapshots record the git commit of the build (`tool.commit`).

### Snapshot schema

0.9.0. Consumers should read `metrics.collected` and treat missing keys as
not measured.
