# miniprof

A lightweight performance profiler for MPI and hybrid MPI + OpenMP
applications on Linux clusters. It attaches to an unmodified, dynamically
linked binary at launch, costs about as much as run-to-run noise, and
writes plain files: a JSON snapshot, a one-page text report, a
self-contained HTML report and CSV tables.

```bash
module load miniprof
mpirun -np 48 --map-by ppr:24:socket:pe=4 --bind-to core miniprof lmp -in in.rhodo -sf omp
```
```
miniprof: lmp, 48 ranks x 4 threads on 1 node(s), 9.8 s: compute 82%, MPI 18% (MPI_Send 7%), I/O 0%
miniprof: report /home/user/mp-test/miniprof_lmp_20261113_122838/report.html
```

## What it tells you

| Question | Where |
|---|---|
| How is the time split between compute, MPI and I/O, and how unevenly across ranks? | time split, imbalance, slowest ranks |
| Which MPI calls cost the most, with what message sizes? | MPI by class and by function, size histograms |
| How much I/O, through POSIX or MPI-IO? | I/O block (calls, bytes, time) |
| Do the OpenMP regions scale? Where do threads wait? | OpenMP regions (serial time, barrier and lock wait, imbalance) |
| Is the core busy, stalled on memory, or mispredicting? | IPC, FLOPs, top-down (frontend, bad speculation, backend, retiring) |
| How much DRAM bandwidth, how much from the other socket, how much power? | memory block per node, socket and channel |
| How far from what the node can do? | roofline against calibrated ceilings |
| Which functions burn the CPU time? | hotspots (`--hotspots`), or the sampling profile (`--sample`) |
| Does it change over the run, or move between ranks? | `miniprof --timeline` |
| Why is run B slower than run A? | `miniprof-diff` |
| What kind of code is this, and how do several codes compare? | fingerprint (in every report), `miniprof-fingerprint` |

Every report ends with **observations**: plain-language findings such as
"MPI takes 49% of the run, mostly MPI_Wait; compute imbalance 35%" or
"ranks are not bound to cores", each with the threshold that triggered it.

## Three modes

| Mode | Use it for | Cost | Rebuild? |
|---|---|---|---|
| **snapshot** (default via the launcher) | Whole-run characterisation in the style of Intel APS and AMDuProfPcm: time split, MPI, I/O, OpenMP, hardware counters, memory, roofline | within run-to-run noise | no |
| **sample** (`miniprof --sample`) | Time per function and source line, per rank; suggests functions worth tracing | low (10 ms SIGPROF) | no |
| **trace** (`MINIPROF_MODE=trace`) | Exact calls, inclusive/exclusive time and counters for chosen functions, plus every MPI call | higher, scoped to what you choose | yes, `-finstrument-functions` |

Start with a snapshot; add `--hotspots` or switch to `--sample` to see
routines; trace only the few functions that matter.

## Quick start

From source, on a login node with your compiler and MPI modules loaded:

```bash
make install PREFIX=$HOME/sw/miniprof/1.1 MODULE_VERSION=1.1
module use $HOME/sw/miniprof/1.1/share/modulefiles
module load miniprof/1.1
miniprof --check ./your_app        # can miniprof attach to it here?
mpirun -np 64 miniprof ./your_app  # profile it
```

Optional, once per installation and node type, for the memory and
roofline sections (see [Installing](docs/installing.md)):

```bash
sudo setcap cap_perfmon+ep $HOME/sw/miniprof/1.1/lib/miniprof-nodemon   # if perf_event_paranoid > 0
srun -N1 --exclusive miniprof-calibrate -o $HOME/sw/miniprof/1.1/share/miniprof/calib
```

## Documentation

| | |
|---|---|
| [Installing](docs/installing.md) | Build, `make install`, the module, the memory helper, calibration, one install per MPI |
| [User guide](docs/user-guide.md) | The `miniprof` launcher, its options and output, profiling without it, regions in your code, C/C++ and Fortran |
| [Snapshot reports](docs/snapshot.md) | Every section of the report: what is measured, how, and how to read it |
| [Hotspots, sampling and tracing](docs/sampling-and-tracing.md) | Finding the hot functions; the sample → select → trace workflow; `mpa-check` |
| [Comparing runs](docs/comparing-runs.md) | `miniprof-diff`, `miniprof-fingerprint`, CSV export, thresholds |
| [Examples](docs/examples.md) | Worked examples on real applications: hybrid MD code, MPI-only ocean model, thread sweep, node comparison |
| [Configuration](docs/configuration.md) | Every environment variable |
| [Hardware support](docs/hardware.md) | What each CPU provides, counter groups, Zen 4/5/6 and Granite Rapids notes, permissions |
| [Development and testing](docs/development.md) | `make` targets, the test suite, validation |
| [Internals](docs/internals.md) | How the code works and how to extend it: lifecycle, subsystems, JSON contract, recipes |
| [Known limitations](docs/limitations.md) | What miniprof does not cover |

## Requirements

- Linux on x86-64; an MPI with development headers (tested with Open MPI
  4.1 and 5.0)
- GCC, Clang, AOCC or Intel oneAPI; gfortran (or another Fortran
  compiler) only for the Fortran examples and wrappers
- Python 3 (standard library only) for the reports
- GNU binutils (`addr2line`, `nm`) for sampling and trace modes
- Hardware counters: `perf_event_paranoid <= 2`. Full detail (top-down,
  memory, energy) is implemented for AMD Zen 4, Zen 5 and Zen 6 and Intel
  Granite Rapids; other CPUs
  get cycles and instructions. Everything else works on any x86-64 Linux.
- OpenMP detail needs a runtime with OMPT: LLVM `libomp` (clang, AOCC) or
  Intel `libiomp5`. GCC's `libgomp` has none.

## Repository layout

| Path | Contents |
|---|---|
| `src/`, `include/` | The library (`libminiprof.so`), the memory helper and the calibration tool |
| `packaging/` | The `miniprof` launcher and the module-file templates |
| `tools/` | Report, HTML, CSV and diff tools; validation and probing scripts |
| `examples/` | Small C and Fortran programs, a 3D diffusion benchmark, and the known-answer validation programs |
| `tests/` | Unit tests, report fixtures |
| `docs/` | This documentation |

## License

(add your license here)
