# Development and testing

- [Building](#building)
- [The test suite](#the-test-suite)
- [Validation on real hardware](#validation-on-real-hardware)
- [Example programs](#example-programs)
- [Source layout](#source-layout)
- [Design notes](#design-notes)

## Building

| Target | Builds |
|---|---|
| `make` / `make all` | the library and all examples |
| `make lib` | `libminiprof.so` |
| `make nodemon` | `miniprof-nodemon`, the memory helper (`make nodemon-setcap` grants it `CAP_PERFMON`, with sudo) |
| `make calibrate` | `miniprof-calibrate` |
| `make mpa-check` | the `.mpa` validator (no MPI dependency) |
| `make examples` | every example, sample and trace variants |
| `make install PREFIX=... MODULE_VERSION=...` | an installation ([installing](installing.md)) |
| `make clean` | removes build artifacts and reports |

`make CC=mpiicc` changes the MPI C compiler wrapper. For the OpenMP tests
with OMPT, build the examples with clang or AOCC
(`OMPI_CC=clang make ...` under Open MPI).

## The test suite

```bash
make check
```

runs everything below and prints `ALL CHECKS PASSED`. Tests that need a
PMU, a capability or an OMPT runtime report `SKIP` (not `FAIL`) where those
are missing, so the suite runs on a laptop, in a VM and in CI. CI runs it
on every pull request with `MPIRUN_FLAGS=--oversubscribe NRANKS=2`.

| Target | Checks |
|---|---|
| `test-units` | MPI-free unit tests: CPU microarchitecture table, JSON writer |
| `run-3d-snapshot` | snapshot of `diffusion3d`, JSON structure |
| `run-hotspots-validate` | hotspots name `diffusion3d`'s inlined kernels (`compute_diffusion` present, `main` not dominant) |
| `run-imbalance-validate` | known answer: injected compute imbalance and Allreduce wait within 2%, MPI time against trace mode within 2%, POSIX, text stdio and MPI-IO byte and call counts exact (including the PnetCDF call pattern) |
| `run-counters-validate` | exact region FLOP counts, thread coverage, compute/MPI split (software-event test anywhere; real counters with a PMU) |
| `run-topdown-validate` | event-set rotation with software events; on a PMU, regions built to be retiring, backend-core, backend-memory and bad-speculation bound |
| `run-memory-validate` | memory helper protocol (test mode, anywhere); known DRAM traffic with the capability |
| `run-calibrate-test` | `miniprof-calibrate --quick` and the roofline plumbing |
| `run-omp-validate` | OpenMP: per-region imbalance, team size, serial time and lock wait against injected values (OMPT runtime) |
| `run-omp-idle-validate` | OpenMP worker spin kept out of compute, and tasks run at a barrier counted as compute: known CPU-seconds (OMPT runtime) |
| `test-ompt-abi` | miniprof's OMPT definitions against the compiler's `<omp-tools.h>` |
| `run-fortran-sentinels` | Fortran `MPI_IN_PLACE` / `MPI_STATUS_IGNORE` results correct in every mode, call counts not doubled |
| `run-hwc-validate`, `-omp`, `-smear` | sample mode's per-function counter classification, per-thread counters, and the interval-smearing warning |
| `run-ext-trace` | trace mode's MPI test/wait and RMA wrappers |
| `test-report-html` | text, HTML, CSV, diff and fingerprint reports against fixtures (`tests/fixtures/`), including escaping; the fingerprint's axes stay within 0-100, its components add up to the run, and its classification rules hold on the fixtures and on synthetic edge cases |
| `run-timeline-validate` | timeline: three known phases (compute only, rank 0 slow, last rank slow) in the MPI series and the heatmap (`examples/timeline_validate`, `tools/validate_timeline.py`) |
| `test-install` | installs into a scratch prefix and runs the launcher: snapshot, `--hotspots`, `--sample`, `--check`, stdin passed to rank 0, `miniprof-csv` and `miniprof-fingerprint`, module files |

`make check` also runs the example workflows (`run`, `run-trace`,
`run-f`, `run-3d`, ...). Separately, `make overhead-3d` measures the
wall-clock cost of snapshot mode on `diffusion3d`.

On a machine with few cores, a handful of timing-based checks can fail
from oversubscription; they pass on a real node.

## Validation on real hardware

The scripts in `tools/` validate a node type end to end
([hardware support](hardware.md#probing-a-node)): counters against
`perf stat`, top-down encodings against perf's, known DRAM traffic, and
the roofline on streaming and FMA kernels. They have been run on EPYC
9754, 9654, 9555 and 9996 nodes.

Snapshot mode was also checked on LAMMPS (rhodopsin, 2 million atoms, MPI
+ OpenMP, 1 and 2 nodes of EPYC 9654): profiled runs within run-to-run
noise of unprofiled ones at every level, identical thermo output and
trajectories, compute and MPI time consistent with LAMMPS's own timing
breakdown, and exact I/O bytes ([examples](examples.md#2-a-hybrid-mpi--openmp-code-lammps)).
The harness for that lives outside this repository.

## Example programs

| Program | Purpose |
|---|---|
| `heat2d` (C), `heat1d` (Fortran) | small introductions to sampling and tracing |
| `diffusion3d` (C) | 3D anisotropic diffusion with a 2D Cartesian decomposition: stencil kernels, directional halo exchanges (`MPI_Sendrecv` with derived types), `MPI_Allreduce` diagnostics. Size with `make example-3d NX=512 NY=512 NZ=256 NSTEPS_3D=500` |
| `*_validate*.c` | known-answer programs behind the tests above |
| `roofline_fma.c`, `mpi_percall.c` | microbenchmarks |

Run targets: `make run` / `run-trace` (`heat2d`), `run-f` / `run-f-trace`
(`heat1d`), `run-3d` / `run-3d-trace` (`diffusion3d`), `make mpa` (the
whole sample → validate → trace workflow). `NRANKS=16` changes the rank
count.

## Source layout

| File | Role |
|---|---|
| `miniprof.c` | init and finalize, mode switch, `MPI_Init`/`MPI_Finalize` interception |
| `miniprof_fortran.c` | Fortran `mpi_init_`/`mpi_finalize_` bindings |
| `mpi_trace.c` | MPI wrappers (C and Fortran) for trace and snapshot modes |
| `io_trace.c` | POSIX, stdio and MPI-IO wrappers |
| `snapshot.c` | snapshot mode: per-rank totals, the finalize gather, the JSON document |
| `counters.c` | hardware counter engine: groups, per-thread counting, compute/MPI/idle split, regions |
| `ompt.c` | the OMPT tool: regions, barriers, locks, worker idle |
| `hotspots.c` | opt-in hotspot sampling for snapshot mode |
| `nodemon.c`, `nodemon_client.c` | the memory helper and its client |
| `calibrate.c` | `miniprof-calibrate` |
| `sysinfo.c` | node and rank metadata, CPU identification |
| `sampler.c`, `sampler_hwc.c`, `profile_store.c`, `path_store.c` | sample mode |
| `aggregator.c`, `mpa_writer.c` | sampling report and `.mpa` suggestions |
| `instrument.c`, `trace_store.c`, `trace_aggregator.c` | trace mode |
| `symbolizer.c` | PC to function and line: in-process ELF symbol tables, `addr2line` |
| `spawn.c` | helper processes without `LD_PRELOAD` |
| `json_writer.c` | streaming JSON |
| `mpa_check.c` | the standalone `mpa-check` |

Reports are Python 3, standard library only: `tools/miniprof_report.py`
(text, observations, thresholds), `miniprof_html.py`, `miniprof_csv.py`,
`miniprof_diff.py`, `miniprof_fingerprint.py` (the fingerprint, used by the
report and the diff, and the side-by-side tool). The fingerprint's
classification thresholds are constants at the top of
`miniprof_fingerprint.py`.

## Design notes

Lessons that shaped the code, worth knowing before changing it:

- **Collective decisions at finalize.** Anything that adds a collective
  call at `MPI_Finalize` must be decided identically on every rank (an
  `Allreduce` of the local decision), never from a rank's own environment:
  a variable that reaches only some ranks would otherwise hang the job.
- **Fortran wrappers forward to the Fortran PMPI bindings.** Fortran's
  `MPI_IN_PLACE`, `MPI_BOTTOM` and `MPI_STATUS(ES)_IGNORE` are addresses
  of library-private variables. An early version converted arguments and
  called the C API, which broke every sentinel silently. Each Fortran
  wrapper now times the call and passes the arguments, untouched, to the
  library's own `pmpi_*_` binding.
- **No double counting.** MPI-IO wrappers raise a tool depth so the POSIX
  calls MPI-IO makes underneath are not counted again; the MPI library's
  own files are excluded.
- **Helpers run without `LD_PRELOAD`.** `addr2line` and `nm` started with
  the profiler preloaded load the whole MPI stack each time (seconds on a
  cold network filesystem). `spawn.c` starts them without it, and
  function names come from the ELF symbol tables in-process where possible.
- **PIE versus non-PIE.** Only PIE executables get a load-base
  correction; adding one to a non-PIE binary made every trace-mode address
  comparison fail silently.
- **Leaf frames.** `backtrace()` returns the interrupted instruction in
  `pcs[0]`; skipping frames there once credited every hot leaf function to
  its caller.
- **OpenMP worker accounting.** LLVM's runtime reports a worker's
  barrier-wait end and implicit-task end only at the next fork, so the
  switch from compute to idle is made at the start of the region's closing
  barrier wait, and explicit tasks run during that wait switch back to
  compute.
- **Counters per OS thread**, opened lazily on each thread's first use;
  reading one thread's counters from another collapses every worker's
  numbers into the master's.
