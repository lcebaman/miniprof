# Known limitations

## MPI coverage

These MPI calls are individually timed, through the C bindings and all
three Fortran interfaces (`mpif.h`, the `mpi` module and the `mpi_f08`
module). Each lands in one class
(`p2p`, `collective`, `wait`, `rma`, `comm`) in the report.

- **point-to-point** (`p2p`): `Send`, `Ssend`, `Rsend`, `Bsend`, `Recv`,
  `Sendrecv`, `Sendrecv_replace`, `Isend`, `Issend`, `Irecv`, `Mrecv`,
  `Imrecv`; persistent `Send_init`, `Ssend_init`, `Rsend_init`,
  `Bsend_init`, `Recv_init`, `Start`, `Startall` (the bytes of a persistent
  request count at every `Start`, not at `*_init`)
- **completion** (`wait`): `Wait`, `Waitall`, `Waitany`, `Waitsome`,
  `Test`, `Testall`, `Testany`, `Testsome`, `Probe`, `Iprobe`, `Mprobe`,
  `Improbe`
- **collectives** (`collective`): `Barrier`, `Bcast`, `Reduce`,
  `Allreduce`, `Gather`, `Gatherv`, `Allgather`, `Allgatherv`, `Scatter`,
  `Scatterv`, `Alltoall`, `Alltoallv`, `Reduce_scatter`,
  `Reduce_scatter_block`, `Scan`, `Exscan`; their nonblocking forms
  (`Ibarrier` ... `Iexscan`), whose post is timed here and whose
  completion is timed in the `Wait`/`Test` call; neighbourhood
  collectives `Neighbor_allgather(v)`, `Neighbor_alltoall(v)` and their
  nonblocking forms
- **one-sided** (`rma`): `Win_create`, `Win_allocate`,
  `Win_allocate_shared`, `Win_create_dynamic`, `Win_free`, `Win_fence`,
  `Win_post/start/complete/wait`, `Win_lock/unlock`,
  `Win_lock_all/unlock_all`, `Win_flush` (`_all`, `_local`,
  `_local_all`), `Win_sync`, `Put`, `Get`, `Accumulate`,
  `Get_accumulate`, `Fetch_and_op`, `Compare_and_swap`, `Rput`, `Rget`,
  `Raccumulate`
- **communicators and topologies** (`comm`): `Comm_dup`, `Comm_split`,
  `Comm_split_type`, `Comm_create`, `Comm_create_group`, `Comm_free`,
  `Cart_create`, `Cart_sub`, `Dist_graph_create(_adjacent)`,
  `Intercomm_create`, `Intercomm_merge`. These move no user data but are
  collective and can take seconds at scale.
- **MPI-IO**: `File_open/close/sync`, `read/write` (plain, `_at`, `_all`,
  `_at_all`, and the nonblocking `i` forms), `set_view`, `set_size`,
  `preallocate`, `set_info`

Not wrapped, so their time counts as compute: MPI-4 persistent
collectives (`Allreduce_init` ...),
partitioned communication, the large-count `_c` functions,
`Alltoallw`, `Comm_idup`, and local calls (groups, datatypes, attributes). Adding a
wrapper is mechanical; see [internals](internals.md#103-wrap-another-mpi-function).

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
  AMD Zen 4, 5 and 6 and Intel Granite Rapids.
  Other CPUs get IPC only. On Granite Rapids the memory block has no
  remote-socket share, and FP16/AMX operations are not counted as FLOPs. See
  [hardware support](hardware.md).
- One counter group per run: IPC/FLOPs (`basic`) and top-down are separate
  runs.
- Top-down gives each thread one event set for the whole run, which
  assumes ranks and threads run similar code (SPMD).
- Memory and energy counters see the whole node, including other jobs.
- The FLOP counter does not separate precisions.
- OpenMP detail needs OMPT (LLVM `libomp` or Intel `libiomp5`), not GCC's
  `libgomp`, and only one OMPT tool can be active in a process.
- The timeline's IPC and GFLOP/s come from the thread that calls MPI,
  at its MPI calls: OpenMP worker threads are not in them, and a long
  stretch without MPI calls is spread evenly. MPI and I/O shares are
  exact per bucket for every thread.
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
