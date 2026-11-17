# Known limitations

## MPI coverage

These MPI calls are individually timed, through the C bindings and the
Fortran `mpi` module / `mpif.h` bindings:

- **point-to-point**: `Send`, `Ssend`, `Rsend`, `Bsend`, `Recv`,
  `Sendrecv`, `Sendrecv_replace`, `Isend`, `Issend`, `Irecv`
- **completion**: `Wait`, `Waitall`, `Waitany`, `Waitsome`, `Test`,
  `Testall`, `Testany`, `Testsome`, `Probe`, `Iprobe`
- **collectives**: `Barrier`, `Bcast`, `Reduce`, `Allreduce`, `Gather`,
  `Gatherv`, `Allgather`, `Allgatherv`, `Scatter`, `Scatterv`, `Alltoall`,
  `Alltoallv`, `Reduce_scatter`, `Reduce_scatter_block`, `Scan`, `Exscan`
- **one-sided (active target)**: `Win_create`, `Win_free`, `Win_fence`,
  `Put`, `Get`, `Accumulate`
- **MPI-IO**: `File_open/close/sync`, `read/write` (plain, `_at`, `_all`,
  `_at_all`, and the nonblocking `i` forms), `set_view`, `set_size`,
  `preallocate`, `set_info`

Not wrapped, so their time counts as compute: nonblocking collectives
(`Iallreduce`, ...), persistent requests, passive-target RMA, communicator
and group management, and **every call made through the Fortran
`mpi_f08` module**. A code built on `mpi_f08` shows its MPI time as compute.
Adding a wrapper is mechanical; see `src/mpi_trace.c`.

## Attaching to an application

- The application must be dynamically linked: a static binary cannot be
  preloaded. Link miniprof in instead.
- It must use the MPI library miniprof was built against (same soname).
  Install one miniprof per MPI stack.
- A run that ends without `MPI_Finalize` (a crash, `MPI_Abort`, a time
  limit) writes no snapshot.
- Non-MPI programs are not profiled.

## Snapshot mode

- Hardware detail (top-down, memory, energy, roofline) is implemented for
  AMD Zen 4, 5 and 6. Other CPUs get IPC only. See
  [hardware support](hardware.md).
- One counter group per run: IPC/FLOPs (`basic`) and top-down are separate
  runs.
- Top-down gives each thread one event set for the whole run, which
  assumes ranks and threads run similar code (SPMD).
- Memory and energy counters see the whole node, including other jobs.
- The FLOP counter does not separate precisions.
- OpenMP detail needs OMPT (LLVM `libomp` or Intel `libiomp5`), not GCC's
  `libgomp`, and only one OMPT tool can be active in a process.
- Hotspots name inlined functions only in binaries built with `-g`;
  otherwise their time appears under the caller.
- The fingerprint's class line comes from fixed rules (documented in
  [snapshot reports](snapshot.md#fingerprint)), not a model of the code.
  It describes the run as a whole: a code with a bandwidth-bound phase and
  a latency-bound phase gets one label. Use [regions](user-guide.md#regions-timing-parts-of-your-code)
  for phase detail.
- The fingerprint's DRAM axis is node-wide, so it includes other jobs on a
  shared node, and it is only as good as the calibration it is measured
  against. Compare fingerprints taken at the same level: a `basic` run has
  no top-down or machine axes.

## Sample and trace modes

- Trace mode needs a rebuild with `-finstrument-functions`.
- Trace mode's exclusive time for one function called concurrently from
  many OpenMP threads is approximate under heavy contention.
- Per-function counters in sample mode are statistical; for short,
  frequently called functions they reflect the sampling interval rather
  than the function (the report detects and warns about this).
- `PROFILER` group overhead grows as `MINIPROF_INTERVAL_MS` shrinks.

## General

- No GPU support: host CPU, MPI, I/O and OpenMP only.
- x86-64 Linux only.
