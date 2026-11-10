# miniprof

A lightweight parallel performance profiler for MPI + C/C++ on Linux. It
runs on any Linux cluster with a standard MPI installation.

Two complementary modes support a sampling → selection → tracing workflow:

- **sample** — statistical PC sampling via `SIGPROF`, near-zero overhead,
  safe to run on every job by default. Produces a hierarchical text
  report grouped by `Group → Function → Source Line`, plus a `.mpa`
  suggestions file listing the hot functions worth tracing in detail.
- **trace** — exact entry/exit instrumentation of the functions named in
  the `.mpa` file, plus per-call MPI timing (call count, time, message
  size). Higher overhead, used only for the functions that matter.

A third mode, **snapshot**, characterises a whole run in the style of
Intel APS and AMDuProfPcm: running totals only, written as one versioned
JSON document. It is under active development — see
[Snapshot mode](#snapshot-mode-in-development).

No GUI. Everything is plain text, designed to be read in a terminal or
piped into a job log.

This repository contains two distinct parts:

| Part | Location | Purpose |
|---|---|---|
| **Library** | `src/`, `include/`, `libminiprof.so`, `modulefiles/` | The profiler to link into your own MPI application (or preload transparently via the bundled Lmod module) |
| **Examples** | `examples/` | Small C and Fortran programs plus a larger scientific benchmark |

If you want to profile your own program, start with [Library](#library).
If you want to try miniprof first, skip to [Examples](#examples).

## Library

### Requirements

- Linux, x86-64
- An MPI implementation with development headers (`mpicc`/`mpifort` on
  `$PATH`) — tested against Open MPI 4.1
- `addr2line`, `nm`, `objdump` (part of GNU binutils, present on every
  normal Linux box)
- GCC or Clang (for `-finstrument-functions` support in trace mode)
- gfortran, only if profiling Fortran MPI codes (see
  [Fortran applications](#fortran-applications))

### Build the library

```bash
make lib
```

This creates `libminiprof.so`. The standalone `.mpa` validator is a
separate optional tool with no MPI dependency:

```bash
make mpa-check
```

The default target, `make`, builds the library and all bundled examples.
See [Examples](#examples) for example-specific targets.

### Transparent sampling via `LD_PRELOAD`

Because miniprof is a shared library, sample mode needs **zero changes**
to your source or build: preload it over an already-built, unmodified
binary and it self-attaches through the same `MPI_Init` PMPI
interception described below.

```bash
mpicc -O2 -g your_app.c -o your_app          # completely normal build
LD_PRELOAD=/path/to/miniprof/libminiprof.so mpirun -n 64 ./your_app
```

This is the intended way to wire miniprof up as an Lmod module: the
module sets `LD_PRELOAD` and does nothing else, so `module load miniprof`
followed by a normal `mpirun` produces a sampling report with no
recompilation or relinking. `-g` is still recommended (see below) for
source-line attribution, but function-level profiling works even without
it, via an `nm`-based fallback — useful when preloading over a binary you
didn't build yourself with debug info.

A ready-to-use module is bundled at `modulefiles/miniprof/1.0.lua`. It
locates `libminiprof.so` relative to its own path, so after `make lib`:

```bash
module use /path/to/miniprof/modulefiles
module load miniprof
mpirun -n 64 ./your_app
```

Trace mode still needs an explicit rebuild (see
[Two-pass profiling workflow](#two-pass-profiling-workflow)), because it
requires the compiler to actually emit `-finstrument-functions` calls
into the target binary — that can't be injected after the fact.

### Link miniprof into your application

You can also link `libminiprof.so` directly, e.g. if you want the binary
to always run under miniprof without relying on `LD_PRELOAD`:

```bash
mpicc -O2 -g your_app.c -L/path/to/miniprof -lminiprof \
    -Wl,-rpath,/path/to/miniprof -Wl,--allow-shlib-undefined \
    -ldl -lm -o your_app
```

`-Wl,--allow-shlib-undefined` is needed because `libminiprof.so`
references a handful of Fortran PMPI symbols (`pmpi_init_` etc., see
[Fortran applications](#fortran-applications)) that only resolve when the
final binary also links Fortran's MPI bindings; some linkers otherwise
treat an indirect shared library's undefined symbols as a hard error at
link time even though they're only ever needed at load time. `-rpath` is
optional — it just saves setting `LD_LIBRARY_PATH` at run time.

That's it — no source changes required. `miniprof` intercepts
`MPI_Init`/`MPI_Init_thread`/`MPI_Finalize` via the standard PMPI
profiling interface and starts/stops itself automatically.

One build requirement to keep in mind:

- `-g` — needed so `addr2line` can resolve source file/line info. Without
  it you'll still get function names, just no line numbers.

`-rdynamic` is **not** required, and miniprof's build no longer uses it.
It's a common reflex to add when working with `backtrace()`/`dladdr()`,
but it only controls which symbols get exported to the *dynamic* symbol
table (`.dynsym`) — and the symbols that actually matter for this
profiler (`static`-qualified compute kernels, internal Fortran `contains`
subroutines) are never eligible for `.dynsym` regardless of the flag.
miniprof resolves those via `nm`/`addr2line` against the regular
`.symtab` instead — see [Notes on symbol resolution](#notes-on-symbol-resolution)
for the full story, including how this was verified empirically rather
than assumed.

For trace mode, build a **second** binary of the same source with
`-finstrument-functions` added. Keep both: sample mode uses the normal
binary, trace mode uses the instrumented one.

```bash
mpicc -O2 -g -finstrument-functions your_app.c \
    -L/path/to/miniprof -lminiprof \
    -Wl,-rpath,/path/to/miniprof -Wl,--allow-shlib-undefined \
    -ldl -lm -o your_app+trace
```

### Fortran applications

Fortran MPI codes work the same way, automatically — no manual init
calls, no separate API. Link your Fortran program against
`libminiprof.so` using `mpifort`:

```bash
mpifort -O2 -g your_app.f90 -L/path/to/miniprof -lminiprof \
    -Wl,-rpath,/path/to/miniprof -Wl,--allow-shlib-undefined \
    -ldl -lm -o your_app
```

For trace mode, build a second instrumented binary exactly as with C:

```bash
mpifort -O2 -g -finstrument-functions your_app.f90 \
    -L/path/to/miniprof -lminiprof \
    -Wl,-rpath,/path/to/miniprof -Wl,--allow-shlib-undefined \
    -ldl -lm -o your_app+trace
```

The bundled `examples/heat1d.f90` program demonstrates this setup; its
build and run commands are listed in the examples section below.

#### Why this needs separate support

Fortran's `mpi`/`mpi_f08` modules do not call the C `MPI_Init` /
`MPI_Finalize` symbols — they call separately-mangled bindings
(`mpi_init_`, `mpi_finalize_`, with a trailing underscore under
gfortran's default mangling) that live in a separate shim library
(`libmpi_mpifh.so` for Open MPI) and bypass the C-level PMPI
interception entirely. Confirmed by disassembling a real gfortran
binary: the call site targets `mpi_init_@plt`, never `MPI_Init`.
`src/miniprof_fortran.c` and the Fortran wrappers in `src/mpi_trace.c`
(and the MPI-IO ones in `src/io_trace.c`) add the matching bindings. Each
one times the call and forwards it, arguments untouched, to the MPI
library's **own Fortran PMPI binding** (`pmpi_allreduce_`, ...).

That forwarding matters. Fortran's `MPI_IN_PLACE`, `MPI_BOTTOM` and
`MPI_STATUS(ES)_IGNORE` are addresses of library-private variables, not
the C constants, and only the MPI library knows them. An earlier version
converted the arguments to C and called the C API instead, which
silently broke every sentinel: `MPI_Allreduce(MPI_IN_PLACE, x, ...)`
returned zeros under miniprof in every mode. `make run-fortran-sentinels`
checks the numerical results of every sentinel-accepting wrapped call
under every mode, and that call counts aren't doubled.

(The Fortran init/finalize wrappers in `src/miniprof_fortran.c` reference
`pmpi_init_` etc., which only exist in `libmpi_mpifh.so` — a library
`mpicc` does not link by default, only `mpifort` does. Since miniprof is
a shared library, that's harmless: the symbols resolve lazily at load
time, so `pmpi_init_` only needs to be found when `mpi_init_` is actually
called, which only happens for Fortran applications. Plain C builds never
trigger the lookup. This is also why linking against `libminiprof.so`
needs `-Wl,--allow-shlib-undefined` — see
[Link miniprof into your application](#link-miniprof-into-your-application).)

#### Optimisation note

gfortran can inline small internal `contains` subroutines aggressively,
removing them as separate symbols and leaving nothing for the profiler
to attribute time to. If a function disappears from a report, try
building at `-O0` or move the hot routine into a separate module
procedure. The bundled Fortran example uses `-O0` for this reason.

#### What's verified working

Both modes were tested end to end against a real Fortran MPI program
(1D heat diffusion with internal `compute_step`/`exchange_halo`/
`check_norm` subroutines): sample mode correctly auto-starts via
`MPI_Init` interception and attributes time down to source line; trace
mode correctly resolves the gfortran-mangled subroutine names
(`compute_step.2`, etc.) via `mpa-check`, and Table 2 (MPI call timing)
is populated from the Fortran-side `MPI_Sendrecv`/`MPI_Allreduce` calls
exactly as it is for C, including correct exclusive-time exclusion of
MPI wait time.

Next, follow the [two-pass profiling workflow](#two-pass-profiling-workflow)
with your sample and trace binaries.

## Examples

The programs in `examples/` demonstrate and test miniprof; they are not
part of the library and are not needed when linking miniprof into your own
application.

| Program | Language | Purpose |
|---|---|---|
| `heat2d` | C | Small, fast introduction to sampling and tracing |
| `heat1d` | Fortran | Exercises the Fortran MPI wrappers |
| `diffusion3d` | C | Larger workload with computation, halo exchange, and collectives |

Build all three sample and trace variants with `make examples`.

### Build and run targets

| Target | What it does |
|---|---|
| `make` / `make all` | Build `libminiprof.so` and all example variants |
| `make lib` | Build `libminiprof.so` only |
| `make examples` | Build all sample and trace example variants |
| `make example` | Build `examples/heat2d` only (requires `lib`) |
| `make example-trace` | Build `examples/heat2d+trace`, instrumented with `-finstrument-functions` |
| `make example-f` | Build `examples/heat1d` (Fortran sample-mode example) |
| `make example-f-trace` | Build `examples/heat1d+trace` (Fortran, instrumented) |
| `make mpa-check` | Build the standalone `.mpa` validator (no MPI dependency) |
| `make run` | Run `heat2d` in sample mode, 4 ranks; prints report and writes `miniprof.mpa` |
| `make run-trace` | Validate `miniprof.mpa` against `heat2d+trace`, then run trace mode, 4 ranks |
| `make run-f` | Fortran equivalent of `run` |
| `make run-f-trace` | Fortran equivalent of `run-trace` |
| `make example-3d` | Build `examples/diffusion3d` (3D anisotropic diffusion, 2D Cartesian MPI) |
| `make example-3d-trace` | Build `examples/diffusion3d+trace` (instrumented) |
| `make run-3d` | Sampling run on `diffusion3d`, 4 ranks |
| `make run-3d-trace` | Validate and trace `diffusion3d` |
| `make example-hwc-validate` | Build `examples/hwc_validate` (Table 2 sanity check, see below) |
| `make run-hwc-validate` | Run `hwc_validate`, 1 rank; asserts the compute-bound/memory-bound kernels land in the expected Table 2 classification |
| `make example-hwc-validate-omp` | Build `examples/hwc_validate_omp` (per-thread counter sanity check, needs `-fopenmp`) |
| `make run-hwc-validate-omp` | Run `hwc_validate_omp`, 1 rank, 2 OpenMP threads running the same two kernels *concurrently*; asserts they're still classified independently and correctly |
| `make example-hwc-validate-smear` | Build `examples/hwc_validate_smear` (interval-smearing warning positive control) |
| `make run-hwc-validate-smear` | Run `hwc_validate_smear`, 1 rank, 20 tiny functions; asserts Table 2's smearing warning fires (opposite check from the two above) |
| `make test-units` | MPI-free unit tests: CPU microarchitecture table, JSON writer |
| `make run-3d-snapshot` | Run `diffusion3d` in snapshot mode and validate the JSON structure |
| `make overhead-3d` | Wall-clock overhead of snapshot mode vs `MINIPROF_MODE=off` (informational) |
| `make run-imbalance-validate` | Snapshot known-answer test: injected imbalance, trace cross-check, exact I/O counts |
| `make run-counters-validate` | Snapshot hardware counters: exact region FLOPs, threads, compute/MPI split (SKIP without a PMU) |
| `make run-omp-validate` | Snapshot OpenMP (OMPT): per-region imbalance, team size, serial time and lock wait vs injected values (SKIP under GCC libgomp; build with `OMPI_CC=clang` or AOCC) |
| `make test-ompt-abi` | miniprof's built-in OMPT definitions vs the compiler's `<omp-tools.h>` (compile-time) |
| `make run-fortran-sentinels` | Fortran `MPI_IN_PLACE`/`MPI_STATUS_IGNORE` results correct in every mode |
| `make mpa` | Run the full `heat2d` workflow: clean, build, sample, validate, trace |
| `make clean` | Remove build artifacts and reports |

Override the rank count with `make run NRANKS=16`, or the MPI compiler
wrapper with `make CC=mpiicc`.

### `diffusion3d`: scientific benchmark

`examples/diffusion3d.c` is the primary profiling benchmark in this
repository. It solves the 3D anisotropic heat/diffusion equation in a
heterogeneous medium:

```
dT/dt = d/dx(Kx dT/dx) + d/dy(Ky dT/dy) + d/dz(Kz dT/dz) + Q
```

where `Kx`, `Ky`, `Kz` are spatially-varying thermal conductivity
fields (modelling geological heterogeneity — high-K inclusions, low-K
zones, anisotropy with `Kz = 0.1 * Kx`) and `Q` is a time-modulated
Gaussian ring heat source. Second-order centred finite differences with
face-averaged conductivities, explicit Euler time integration, Dirichlet
BCs on Z faces, periodic in XY.

**Parallelism**: 2D Cartesian MPI topology (`MPI_Cart_create` with
`MPI_Dims_create` auto-factorisation). X and Y dimensions are decomposed
across ranks; each rank owns the full Z extent. Halo exchange uses
`MPI_Sendrecv` with MPI derived datatypes (`MPI_Type_contiguous` for YZ
faces, `MPI_Type_vector` for XZ faces). `MPI_Allreduce` gathers global
L2-norm, min, and max temperature for diagnostics every `DIAG_INTERVAL`
steps.

**Hot spots visible to miniprof**:

| Function | What it does |
|---|---|
| `compute_diffusion` | 7-point stencil inner loop, face-averaged variable K |
| `update_source` | time-modulated volumetric heat source accumulation |
| `halo_exchange_x` | MPI face exchange in X direction only |
| `halo_exchange_y` | MPI face exchange in Y direction only |
| `advance` | explicit Euler update of T |
| `apply_bc` | Dirichlet BC on top/bottom Z planes |
| `compute_diagnostics` | global L2/min/max via 3 × MPI_Allreduce |

The X and Y halo exchanges are deliberately kept as separate functions
so the profiler can distinguish directional communication imbalance —
relevant on real clusters where mesh decompositions are rarely square.

**Build**:

```bash
make example-3d                                     # 256x256x128, 200 steps (default)
make example-3d NX=512 NY=512 NZ=256 NSTEPS_3D=500  # larger production run
make example-3d-trace                               # instrumented binary
```

**Two-pass profiling workflow**:

```bash
make run-3d        # sampling pass → miniprof_report_3d.txt + miniprof_3d.mpa

# review and optionally extend miniprof_3d.mpa:
echo "halo_exchange_x" >> miniprof_3d.mpa
echo "halo_exchange_y" >> miniprof_3d.mpa
echo "advance"         >> miniprof_3d.mpa
echo "apply_bc"        >> miniprof_3d.mpa

./mpa-check examples/diffusion3d+trace miniprof_3d.mpa  # validate before submitting
make run-3d-trace  # tracing pass → miniprof_trace_report_3d.txt
```

**Trace report on 128x128x64 grid, 4 ranks**:

```
Table 1: User Function Tracing
   Calls    Incl(s)    Excl(s)    Avg.Excl    Function
     400      0.925      0.925    2312us    compute_diffusion    <- stencil dominates
     400      0.200      0.200     501us    update_source
     400      0.179      0.179     447us    advance
     400      0.830      0.001       1us    halo_exchange_x      <- nearly all MPI wait
     400      1.018      0.001       1us    halo_exchange_y      <- halo is the bottleneck

Table 2: MPI Call Tracing
   Calls    Time(s)    Avg(us)    Total Bytes    Function
    1600      1.847    1154us      52428800      MPI_Sendrecv    <- ~45% of wall time
      72      0.061     850us           576      MPI_Allreduce
```

The exclusive time split (`halo_exchange_x` exclusive ~0.001s while
`MPI_Sendrecv` totals ~1.85s) shows this run is communication-bound at
this rank count and grid size — the correct signal for deciding whether
to increase local work per rank (larger NX/NY/NZ) or rethink
communication overlap.

## Library workflow and reference

### Two-pass profiling workflow

This is the recommended way to use miniprof: sample broadly, select the
important functions, then trace those functions precisely.

#### 1. Sample first

Run your normal binary with sampling enabled (the default mode — no env
vars required beyond pointing at an output file if you want a specific
name):

```bash
mpirun -n 64 ./your_app
```

This produces `miniprof_report.txt` — a flat sampling profile,
grouped by `USER` / `MPI` / `OMP` / `LIBC` / `SYSTEM` / `ETC`, broken
down to function and source-line granularity, with per-rank load
imbalance — and `miniprof.mpa`, a plain-text list of every `USER`
function whose sampled time exceeded the threshold (default 1%).

```bash
$ cat miniprof.mpa
# miniprof MPA suggestions — generated 2026-06-30 14:38:44
# Functions with Samp% >= 1.0% from the sampling pass.
# Only USER-group functions are listed: MPI calls are
# always traced automatically (see mpi_trace.c) and don't
# need — and can't use — an entry here.
# Edit this file to add/remove functions, then run:
#   ./mpa-check <binary>+trace miniprof.mpa   (validate first!)
#   MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=miniprof.mpa mpirun -n <N> ./your_app+trace
# One function name per line; '#' starts a comment.
#
# Function                             Samp%
compute_stencil                     #   9.1%
check_convergence                   #   7.2%
```

Feel free to hand-edit this file — add functions you know you care about
even if they didn't clear the sampling threshold, or remove ones you
don't want traced.

#### 2. Validate before you trace

Before submitting a (possibly expensive, many-node) tracing run, check
that every function in the `.mpa` file will actually resolve in your
instrumented binary:

```bash
./mpa-check your_app+trace miniprof.mpa
```

```
mpa-check: validating 2 function(s) from 'miniprof.mpa' against 'your_app+trace'

  OK    compute_stencil                           0x00003be0  (local/static)
  OK    check_convergence                         0x00003e50  (local/static)

2 resolved, 0 failed, out of 2 total

mpa-check: confirmed 4 instrumentation call site(s) in 'your_app+trace' — this binary was built with -finstrument-functions.

mpa-check: all functions resolved successfully.
```

Exit code is `0` on success, `1` on any failure — safe to use as a gate
in a job script:

```bash
./mpa-check your_app+trace miniprof.mpa || exit 1
mpirun -n 64 ./your_app+trace
```

If something doesn't resolve, `mpa-check` tells you why:

```
  FAIL  compute_stenncil                          (not found in binary)

mpa-check: FAILED — 1 function(s) will not be traced.
  Common causes:
    1. Typo in the .mpa file
    2. Function was inlined away or optimized out
       (try -O0 or __attribute__((noinline)) on that function)
    3. .mpa file was generated against a different binary
    4. Binary is stripped (rebuild without -s / strip)
```

It also independently checks that the binary you're pointing it at was
genuinely built with `-finstrument-functions` (not just linked against
the library) — a resolved name doesn't guarantee an entry/exit hook will
actually fire if the compiler flag was forgotten.

#### 3. Trace

```bash
MINIPROF_MODE=trace \
MINIPROF_TRACE_FUNCS=miniprof.mpa \
mpirun -n 64 ./your_app+trace
```

Produces `miniprof_trace_report.txt`: exact call counts, inclusive time,
and exclusive (self) time for each traced function, plus a separate
table of MPI call statistics (call count, total time, message bytes) —
MPI tracing happens automatically, no `.mpa` entry needed.

```
Table 1: User Function Tracing (aggregated across all ranks)
   Calls   Incl(s)   Excl(s)   Avg.Excl   MaxCalls  Function
    1600    7.72      7.72     4826.38us    400     compute_stencil
    1600   11.86      7.28     4552.68us    400     check_convergence

Table 2: MPI Call Tracing (aggregated across all ranks)
   Calls   Time(s)   Avg(us)   Total Bytes   Avg Bytes  Function
    3200    5.75      1796.49   52428800     16384.0    MPI_Sendrecv
    1600    4.58      2860.74     12800          8.0    MPI_Allreduce
```

Exclusive time correctly excludes time spent inside MPI calls — note
`check_convergence`'s exclusive time (7.28s) is noticeably lower than
its inclusive time (11.86s), with the ~4.58s difference matching almost
exactly the total `MPI_Allreduce` time in Table 2, since nearly all of
that gap is `MPI_Allreduce` wait time, already broken out separately.

If hardware counters are available (see
[Table 2: Hardware Counter Characterisation](#table-2-hardware-counter-characterisation)
in the sampling report), the trace report gains a third table with
**exact per-invocation** IPC and cache-miss data — `perf_event_open`
counters are read at every traced function's entry and exit, so unlike
sample mode's statistical leaf-PC attribution, there's no smearing:

```
Table 3: Hardware Counter Characterisation (exact per-invocation, inclusive)
   Calls    IPC    MPKI      Cycles  Instructions  LLC misses  Classification  Function
    1600   3.68    4.47   1.095e+10     4.028e+10   1.799e+08  COMPUTE-BOUND   compute_stencil
    1600   1.51   13.28   8.997e+09     1.356e+10   1.802e+08  COMPUTE-BOUND   check_convergence
```

Same classification rule and thresholds as sample mode's Table 2 (IPC
and MPKI, just calls instead of samples). Two differences from sample
mode's version worth knowing:

- **Inclusive, not exclusive.** These counters are the entry→exit delta
  for the whole call, including any nested traced functions or plain
  (non-traced) work — the same convention as `Incl(s)` in Table 1, not
  `Excl(s)`. A function whose IPC looks lower than expected here may
  simply be dominated by a callee's counter activity.
- **Exact, not statistical.** Because these numbers come from real
  per-call boundaries rather than samples landing on a leaf PC, they can
  (and did, during testing) disagree numerically with Table 2's
  statistical estimate for the same function — that's expected, and
  Table 3's numbers are the more precise ones when both are available.

Only traced functions get a row (the `.mpa`-selected set), the same
scoping as Tables 1 and 2. If any rank lacked hardware counters, this
table is replaced by an explanatory note instead of showing zeros — see
[Table 2](#table-2-hardware-counter-characterisation) for why that can
happen.

### Snapshot mode (in development)

```bash
MINIPROF_MODE=snapshot mpirun -n 64 ./your_app       # no rebuild needed
python3 tools/miniprof_report.py miniprof_snapshot.json
```

Snapshot mode is a low-overhead, whole-run characterisation in the
spirit of Intel APS and AMDuProfPcm. It records no individual samples or
calls: each rank keeps running totals, and at `MPI_Finalize` they are
gathered into one versioned JSON document (`miniprof_snapshot.json`).
Every report is generated from that file, so the profiler itself stays
small. `tools/miniprof_report.py` prints a one-page summary (here for
`make run-imbalance-validate`, 2 ranks with injected imbalance):

```
 Time split (average over ranks)
   Compute      74.6%  ######################........
   MPI          24.8%  #######.......................
   I/O           0.6%  ..............................

 Compute imbalance      25.0%   (perfect balance would save 24.8% of elapsed)
   slowest ranks       1 (801.7 ms), 0 (401.6 ms)
 MPI time imbalance     50.0%   (min 288 us, max 400.5 ms)
 CPU utilisation        98.9%   (min 98.6%, max 99.1%)

 MPI by class           time%    calls        data
   collective           24.8%        84       640 B

 Top MPI functions      time%    imb%    calls   avg size
   MPI_Allreduce        24.8%   50.0%       80        8 B
   MPI_Barrier           0.0%   40.2%        4        0 B

 Message sizes
   collective   most calls: 8 B-16 B (95%);  most time: 8 B-16 B

 I/O                    read                 write             time (avg/rank)
   POSIX       3.0 MiB (    24 calls)    3.0 MiB (    24 calls)   3.2 ms
   MPI-IO    512.0 KiB (     2 calls)  512.0 KiB (     2 calls)   1.6 ms

 Observations
   ! MPI takes 24.8% of the run (> 20%); largest contributor MPI_Allreduce.
   ! Compute is imbalanced across ranks (25.0% > 10%); much of the MPI wait time is likely ranks waiting for the slowest ones.
```

What a snapshot contains so far:

- **run / nodes / ranks**: command, rank and node counts, CPU
  microarchitecture straight from CPUID (`zen4`, `zen5`,
  `granite_rapids`, ...), NUMA, SMT, kernel, `perf_event_paranoid`, CPU
  affinity per rank, MPI library and thread level, and a hint for
  whether the node was shared
- **metrics.parallel**:
  - time split into compute, MPI and I/O per rank (compute = elapsed −
    MPI − I/O), with min/avg/max, imbalance `(max − avg) / max`, the
    share of elapsed time perfect balance would save, and the slowest
    ranks
  - MPI time by class (point-to-point, collective, wait/test, RMA) and by
    function, with call counts, bytes and per-function imbalance
  - message-size histograms per class (power-of-two bins: calls and time)
  - POSIX I/O (`open`/`read`/`write`/`pread`/`pwrite`/`fread`/`fwrite`/
    `fsync`, including `_FORTIFY_SOURCE` variants) and MPI-IO, kept
    separate: calls, bytes, read/write/metadata time. Only files opened
    by the application on a regular path are counted; the MPI library's
    own I/O is excluded
  - CPU utilisation (CPU time ÷ elapsed × CPUs the rank may run on),
    user/system time, peak RSS, context switches
- **overhead**: the profiler's own init/finalize cost

- **metrics.counters** (hardware counters, AMD Zen 4 / Zen 5; other CPUs
  get cycles and instructions only):
  - one counter group per run, chosen by `MINIPROF_SNAPSHOT`: `basic`
    collects cycles, instructions and FLOPs, plus (if they fit) L1D fills
    from DRAM, from a remote NUMA node, and L2 misses; `roofline` collects
    FP operations by width (scalar, 128, 256, 512-bit) and cycles
  - Zen has 6 general-purpose counters per thread, the FLOP event takes
    two, and the NMI watchdog (on by default on RHEL-family systems)
    takes one more. Optional events are dropped from the end of the group
    until it fits, and the JSON says which were dropped
  - counts are user-space only (works at `perf_event_paranoid=2`), per
    thread, including threads created after `MPI_Init`, and split into
    outside-MPI and inside-MPI using reads at every MPI call (`rdpmc`
    when allowed, about 20 ns; `read()` otherwise)
  - derived: IPC (overall, outside MPI, inside MPI), effective clock,
    GFLOP/s, FLOPs per cycle, L2 misses per 1000 instructions, DRAM fill
    rate (a lower bound: lines the L2 prefetcher brings in are not seen)
    and remote-NUMA share, FP width mix (the width events count every
    FP-unit uop including loads, stores and moves, so the mix shows vector
    instruction usage rather than the vectorised share of arithmetic)
  - **regions**: bracket code with `miniprof_region_begin("name")` /
    `miniprof_region_end("name")` (Fortran: `CALL
    MINIPROF_REGION_BEGIN('name')`) to get calls, time and counters per
    region, reduced across ranks. No-ops outside snapshot mode

- **metrics.openmp** (OpenMP, through the OMPT tools interface):
  - serial time (outside any parallel region, on the initial thread) and
    parallel time per rank
  - thread time split into work, barrier wait (explicit barriers and the
    implicit ones at the end of worksharing loops and parallel regions),
    lock wait (`critical`, `omp_set_lock`, ...) and taskwait
  - imbalance = barrier wait ÷ team thread time, overall and per parallel
    region; each region is named after the function containing it, with
    file:line when the binary has `-g`, and reduced across ranks
  - needs an OpenMP runtime with OMPT: LLVM `libomp` (clang, AOCC) or
    Intel's `libiomp5` (ICX). GCC's
    `libgomp` has no OMPT; the block then says `available: false` with
    the reason, and everything else in the snapshot is unaffected.
    miniprof registers itself as the OMPT tool by setting
    `OMP_TOOL_LIBRARIES` at load time, so no extra step is needed; set
    `MINIPROF_OMPT=0` to leave OpenMP alone. A process has one OMPT
    tool: if another tool is already listed in `OMP_TOOL_LIBRARIES` and
    accepts, it wins and the block reports why OpenMP was not measured.
  - validation: `make run-omp-validate` injects serial time, balanced and
    imbalanced regions, a contended critical section and an imbalanced
    static loop; imbalance per region is within 3 points of the analytic
    value, wall times within 5%

`topdown`, `memory` and `roofline` are still `null`; they arrive in later
phases. `MINIPROF_SNAPSHOT=topdown` and `full` currently collect the
`basic` group.

Counter validation on a real node: `tools/pmu_probe.sh` records what the
node's PMU and perf support; `tools/validate_counters.sh` checks exact
region FLOP counts, thread coverage and the compute/MPI split against a
known-answer workload and `perf stat`. `make run-counters-validate` runs
the same checks plus a software-event test of the engine that works on
machines without a PMU (CI).

Validation: `make run-imbalance-validate` runs a known-answer workload
(rank r computes `base + r·delta` per step, then a fixed POSIX and MPI-IO
volume). Per-rank compute time, the imbalance and the Allreduce wait
match the injected values within 2%, MPI time matches trace mode within
2%, and I/O byte and call counts are exact. Wrapper cost is about 95 ns
per MPI call; end-to-end overhead on a 20 s `diffusion3d` run is within
run-to-run noise (`make overhead-3d`).

### Configuration

All of these are read at `MPI_Init` time; set them in your job script or
`mpirun -x VAR` (or just `export` before `mpirun`, depending on your MPI
launcher's environment-passing convention).

| Variable | Mode | Default | Meaning |
|---|---|---|---|
| `MINIPROF_MODE` | all | `sample` | `sample`, `trace`, `snapshot`, or `off` (library loaded but inactive) |
| `MINIPROF_SNAPSHOT` | snapshot | `basic` | Counter groups to collect: `basic`, `topdown`, `roofline`, or `full` |
| `MINIPROF_OMPT` | snapshot | `1` | `0` disables OpenMP measurement (OMPT) |
| `MINIPROF_OMPT_DEBUG` | snapshot | unset | Set to trace every OMPT callback on stderr (debugging only; very verbose) |
| `MINIPROF_COUNTERS_SPLIT` | snapshot | `1` | `0` stops reading counters at MPI calls: no inside/outside-MPI split, but no per-call cost either |
| `MINIPROF_SNAPSHOT_OUTPUT` | snapshot | `miniprof_snapshot.json` | Snapshot JSON path |
| `MINIPROF_INTERVAL_MS` | sample | `10` | SIGPROF sampling interval, milliseconds |
| `MINIPROF_DEPTH` | sample | `8` | Call-stack depth captured per sample |
| `MINIPROF_OUTPUT` | sample | `miniprof_report.txt` | Sampling report path |
| `MINIPROF_MPA_OUTPUT` | sample | `miniprof.mpa` | Where to write the trace-candidate suggestions |
| `MINIPROF_MPA_THRESHOLD` | sample | `1.0` | Minimum Samp% for a USER function to be suggested |
| `MINIPROF_TRACE_FUNCS` | trace | `miniprof.mpa` | Which `.mpa`-format file to read the trace function list from |
| `MINIPROF_TRACE_OUTPUT` | trace | `miniprof_trace_report.txt` | Tracing report path |
| `MINIPROF_IPC_COMPUTE_THRESHOLD` | both | `1.2` | IPC at/above this → COMPUTE-BOUND (hardware-counter tables) |
| `MINIPROF_IPC_MEMORY_THRESHOLD` | both | `0.5` | IPC at/below this + high MPKI → MEMORY-BOUND |
| `MINIPROF_MPKI_MEMORY_THRESHOLD` | both | `10` | MPKI (LLC misses per 1000 instructions) at/above this, combined with low IPC, → MEMORY-BOUND |
| `MINIPROF_VERBOSE` | both | `0` | Set to `1` for per-rank diagnostic lines on stderr |

The `.mpa` file format is plain text: one function name per line,
`#` starts a comment (also used for trailing inline annotations).
Blank lines are ignored. It's meant to be hand-edited.

### Reading the sampling report

```
Table 1: Profile by Group, Function, and Line
   Samp%      Samp   Imb.Samp  Imb.Samp%  Group
                                           Function
                                            Source Line
----------------------------------------------------------------------------------------
  100.0%      9166         --         --  Total
   49.4%      4529         --         --  LIBC
   16.4%      1505       44.8      10.6%   __sigaction
   16.4%      1505         --         --  PROFILER
   16.4%      1505       44.8      10.6%   sigprof_handler
   16.4%      1503         --         --  USER
    9.1%       832       29.0      12.2%   compute_stencil
    2.1%       190         --         --    heat2d.c:38
    1.9%       172         --         --    heat2d.c:33
    7.2%       659       16.2       9.0%   check_convergence
    2.9%       263         --         --    heat2d.c:68
   16.3%      1490         --         --  SYSTEM
    1.4%       127         --         --  MPI
```

The table groups samples by category, function, and source line:

- **Samp% / Samp** — fraction and absolute count of samples landing in
  this group/function/line, aggregated across all ranks.
- **Imb.Samp / Imb.Samp%** — load imbalance, computed as
  `Imb.Samp = max_rank_samples − mean_rank_samples`,
  `Imb.Samp% = Imb.Samp / max_rank_samples × 100`. A function with a
  large `Imb.Samp%` is unevenly distributed across ranks — a likely
  candidate for load-balancing work.
- **Group** — `USER` (your code), `MPI`, `OMP`, `LIBC`, `SYSTEM`
  (process startup), `PROFILER` (miniprof's own sampling overhead —
  see below), or `ETC` (anything unclassified).

Below the group/function table are a per-rank top-5 breakdown and the
raw per-rank sample counts and wall-clock times.

#### Table 2: Hardware Counter Characterisation

If the kernel allows it (`perf_event_paranoid` ≤ 2, or `CAP_PERFMON`),
each sample also accumulates CPU cycles, retired instructions, and
last-level-cache misses via `perf_event_open`, and the report gains a
second table classifying every **USER**-group function as compute-bound
or memory-bound. Scoped to `USER` deliberately: an MPI progress engine
or `libc`/OpenMP-runtime function that happens to catch a sample isn't
code you can act on here, and mixing it in just buries the functions
that are (Table 1 still shows every group, since that's useful there —
it's specifically this table that's about *your* code's characteristics):

```
Table 2: Hardware Counter Characterisation (per function)
------------------------------------------------------------------------------------------
      Samp     IPC    MPKI      Cycles  Instructions  LLC misses  Classification  Function
------------------------------------------------------------------------------------------
       832    1.83    0.09   9.480e+08     1.734e+09   1.561e+05  COMPUTE-BOUND   compute_viscous_dissipation
       659    0.42   28.14   2.782e+09     1.168e+09   3.288e+07  MEMORY-BOUND    compute_stencil
------------------------------------------------------------------------------------------
  Classification: IPC>=1.2 = COMPUTE-BOUND, IPC<=0.5 + MPKI>=10.0 = MEMORY-BOUND, else MIXED
```

- **IPC** — instructions retired per cycle for that function, summed
  across all ranks and all samples. High IPC means the pipeline is
  staying full; low IPC means the CPU is stalled, usually waiting on
  memory.
- **MPKI** — last-level-cache misses per 1000 instructions retired,
  the standard architecture metric for cache-miss rate. Normalizing by
  instructions (the same denominator IPC uses, since `IPC = instructions
  / cycles`) rather than by sample/call count keeps the two numbers on a
  comparable, hardware-meaningful scale — and keeps the values in a
  human-recognizable range (single or low double digits for a
  reasonably cache-friendly function) instead of the arbitrary
  large numbers a per-sample or per-call normalization produces.
- **Cycles / Instructions / LLC misses** — the raw totals IPC and MPKI
  are computed from, summed across all ranks and all samples, shown in
  scientific notation. Useful for sanity-checking the derived numbers,
  or if you want a different normalization than IPC/MPKI.
- **Classification** — a heuristic from IPC and MPKI; the three cutoffs
  are architecture-dependent and configurable via
  `MINIPROF_IPC_COMPUTE_THRESHOLD`, `MINIPROF_IPC_MEMORY_THRESHOLD`, and
  `MINIPROF_MPKI_MEMORY_THRESHOLD` (defaults: 1.2, 0.5, 10 — tune these
  per CPU generation; the "right" MPKI to call memory-bound varies by
  memory subsystem and cache size).

The counter deltas between consecutive samples are attributed to
whichever leaf PC the *second* sample landed on — an approximation, not
exact per-instruction accounting, but one that converges to a
representative IPC/miss-rate given enough samples for a function **that
runs for longer than one sampling interval**. When that assumption
holds it's a good approximation, validated against `perf stat` on
`heat2d`'s `compute_stencil`.

It breaks down for functions shorter than `MINIPROF_INTERVAL_MS`: a
single 10ms window can span thousands of calls to many small, frequently
called subroutines (a tight numerical solver loop, say), and the *whole
window's* counter delta gets attributed to whichever one the sample
happened to land on — not because of a bug, but because that's what
"delta since the last sample" fundamentally measures. The tell: many
structurally unrelated functions all showing nearly the same cycles per
sample (`Cycles` ÷ `Samp` — not a displayed column, but easy to check by
eye, and exactly what the detector below computes internally), because
none of them are really measuring their own cost anymore — they're all
measuring "however many cycles elapsed in one sampling interval."
Confirmed against a real MPI+OpenMP NAS Parallel Benchmark run, where
this fired for exactly this reason.

**Table 2 detects this itself** and warns instead of silently presenting
numbers that look plausible but aren't:

```
  WARNING: 21 of 25 functions above (84%) show Cyc/samp within 20% of
  each other (interval smearing)
  These numbers likely reflect "cycles per 10ms sampling interval", not
  this function's own cost — a sign that these
  functions run for less time than MINIPROF_INTERVAL_MS=10, so a single
  interval spans many different calls and
  the whole interval's counter delta gets attributed to whichever one
  the sample happened to land on. IPC/MPKI
  numbers above are not trustworthy for those functions. For exact
  per-function data regardless of call duration, use
  trace mode (Table 3) instead — see the two-pass profiling workflow in
  the README.
```

The check: among functions with at least a handful of samples, compute
cycles-per-sample internally (not a displayed column) and take the
median, then see how many functions land within 20% of it. A tight
majority (≥60%, requiring at least 5 functions) is the smearing
signature — but that clustering alone isn't quite sufficient
evidence with only a few data points, since genuinely different
functions can coincidentally land close together by chance (caught in
testing: two deliberately opposite kernels, IPC 1.96 vs 0.01, landed in
the same band with only 5 qualifying functions). Real smearing also
produces near-uniform *IPC* across the clustered set — the interval is
dominated by whichever code actually ran, and that same code's
instruction mix drives both numbers together — so the warning
additionally requires ≥90% of the clustered functions to share the same
classification (COMPUTE-BOUND/MEMORY-BOUND/MIXED). Genuinely different
functions rarely do; smeared ones almost always do. Both conditions are
exercised directly: `make run-hwc-validate-smear` is a positive control
(many tiny functions — the warning should fire), while
`run-hwc-validate`/`run-hwc-validate-omp` are negative controls (long-
running, structurally different kernels — it should stay silent).

If hardware counters aren't available on any rank (denied by
`perf_event_paranoid`, or no PMU — e.g. inside a VM without a virtual
PMU), sampling proceeds completely normally and Table 2 is replaced with
a one-line note instead of silently showing zeros. A one-time diagnostic
is also printed to stderr (rank 0 only) at startup in that case.

### Notes on symbol resolution

Two non-obvious things this profiler had to work around, worth knowing
if you're debugging unexpected output:

**`dladdr()` cannot see `static`/local functions, ever, regardless of
`-rdynamic`.** That flag only controls which symbols get *exported* to
the dynamic symbol table (`.dynsym`) for cross-DSO resolution — local
symbols are never eligible no matter what. Since HPC compute kernels
are very often declared `static` (and Fortran's internal `contains`
subroutines are conceptually the same thing), relying on `dladdr` alone
would silently fail to symbolize most of the interesting code. miniprof
works around this in two places: the sampling symbolizer uses
`addr2line` (which reads the regular `.symtab`, not `.dynsym`) for
anything `dladdr` can't name; the trace-mode instrumentation filter
resolves the `.mpa` function list to addresses once at startup via `nm`
against the binary itself, rather than trying to resolve incoming PCs
at runtime.

A direct consequence: **`-rdynamic` is not required and miniprof's own
build does not use it.** This was verified empirically, not assumed —
identical sample and trace reports were produced with and without the
flag across both the C and Fortran examples, since the symbol paths
that actually carry weight here (`nm`, `addr2line`) read `.symtab`
directly off the ELF file and never depend on `.dynsym` at all. The
remaining `dladdr()` call sites in `symbolizer.c` still run regardless
of `-rdynamic` — they're used for DSO path and load-base resolution
(`dli_fname`/`dli_fbase`), which `dladdr` derives from the process's
memory-mapping data, not from `.dynsym`, so they succeed either way; only
`dli_sname` (the symbol *name*) ever needed `.dynsym`, and that's
precisely the part with an `addr2line` fallback already in place.

**PIE binaries need load-base correction — non-PIE binaries must NOT get
one.** A PIE (`ET_DYN`) binary loads at a runtime-chosen address, so the
addresses `nm`/`addr2line` print (link-time addresses, relative to the
binary's own coordinate system) don't match the addresses you see at
runtime (`backtrace()`, `__cyg_profile_func_enter`'s `this_fn` argument)
until you add the load base the loader actually placed it at. A non-PIE
(`ET_EXEC`) binary loads at its *fixed* link-time address instead — `nm`'s
addresses are already the runtime addresses, and adding a load base on
top double-counts it, so every comparison silently fails to match.

This isn't a hypothetical: this exact bug shipped and was only caught
during testing of the hardware-counter trace-mode work, because trace
mode's function-address matching in `instrument.c` unconditionally added
`/proc/self/maps`'s load base regardless of binary type — and this
toolchain's `mpicc` produces `ET_EXEC` (non-PIE) binaries by default, not
PIE. The result was silent, not a crash: every traced function's address
comparison failed, `__cyg_profile_func_enter`/`_exit` were called
correctly but never matched, and trace mode reported real MPI timing
(Table 2, wrapped independently in `mpi_trace.c`) but **zero** user
functions (Table 1) on every run — with no error, since a `.mpa` file
listing legitimately-resolvable functions produces no unresolved-symbol
warning at `instrument_load_trace_list()` time (that check validates
symbol names against `nm`'s table, not against runtime address matching).
`mpa-check` didn't catch it either, for the same reason: it validates
symbol names, not runtime address matching. Fixed by reading the ELF
header's `e_type` field directly (`instrument.c`'s `is_pie_executable()`)
and only adding the `/proc/self/maps` load base for `ET_DYN` binaries;
`ET_EXEC` binaries now correctly use a load base of 0. Sample mode's
`dladdr()`-based path was never affected — `dli_fbase` already returns 0
for non-PIE executables, so it happened to do the right thing by
construction.

If you ever see a `??` symbol with surprisingly high sample weight, or a
function from your `.mpa` file failing to resolve when you're sure the
name is right, these are the first two things to suspect — and
`mpa-check` is specifically built to catch the second case before you
waste a cluster allocation on it.

**A function that should clearly be hot showing zero samples.**
`sampler.c`'s `SIGPROF` handler walks the call stack via `backtrace()`
and previously skipped its first three returned frames, under the
assumption that `backtrace()` includes a frame for itself in the
array it returns. It does not — `pcs[0]` is already the true leaf, the
actual interrupted instruction. The old code's extra skip silently
discarded every sample's leaf frame and started attribution one level
up the call stack, which meant any function whose own body was the hot
path (rather than its caller) received zero direct credit, no matter
how much CPU time it actually consumed — confirmed by direct
experiment: a function measured at 1.65s of real CPU time across 100
calls received zero sampling hits under the old code, while raw
`backtrace()` calls in an identical standalone test landed inside that
same function on 74% of samples. This is fixed; `sigprof_handler` now
records starting from the true leaf frame. One side effect: since the
handler's own epilogue can occasionally be the "true leaf" at the
instant of signal delivery, a small fraction of samples legitimately
land inside `sigprof_handler` itself — these are classified under their
own `PROFILER` group (rather than mixed into `USER`) specifically so
they're visible as profiling overhead without polluting your own code's
numbers.

### Known limitations

- Only the MPI calls wrapped in `mpi_trace.c` are individually timed
  (C and Fortran `mpi`/`mpif.h` bindings): point-to-point `Send`,
  `Ssend`, `Rsend`, `Bsend`, `Recv`, `Sendrecv`, `Sendrecv_replace`,
  `Isend`, `Issend`, `Irecv`; completion `Wait`, `Waitall`, `Waitany`,
  `Waitsome`, `Test`, `Testall`, `Testany`, `Testsome`, `Probe`,
  `Iprobe`; collectives `Barrier`, `Bcast`, `Reduce`, `Allreduce`,
  `Gather`, `Gatherv`, `Allgather`, `Allgatherv`, `Scatter`, `Scatterv`,
  `Alltoall`, `Alltoallv`, `Reduce_scatter`, `Reduce_scatter_block`,
  `Scan`, `Exscan`; and active-target RMA `Win_create`, `Win_free`,
  `Win_fence`, `Put`, `Get`, `Accumulate`. Anything not wrapped
  (nonblocking collectives, passive-target RMA, communicator management,
  and the `mpi_f08` module's bindings) falls through to the real MPI call
  with zero overhead and zero report entry; in snapshot mode its time
  counts as compute. Extending coverage is mechanical; see the pattern
  in `mpi_trace.c`.
- Trace mode's exclusive-time accounting for OpenMP+MPI hybrid codes
  with concurrent calls into the *same* traced function from multiple
  threads is a documented approximation (rare lost updates under heavy
  contention) — fine for typical HPC codes, not appropriate if you need
  hard guarantees under heavy threading.
- No GPU/CUDA support. This profiles host-side CPU and MPI activity
  only.
- Trace mode cannot be enabled via `LD_PRELOAD` alone — instrumentation
  calls are inserted by the compiler at `-finstrument-functions`
  build time, so the target binary must be rebuilt (or explicitly linked
  against `libminiprof.so`, see
  [Two-pass profiling workflow](#two-pass-profiling-workflow)). Only
  sample mode is fully transparent.
- Hardware performance counters cover cycles, instructions, and
  cache-misses (sample mode's
  [Table 2](#table-2-hardware-counter-characterisation) and trace mode's
  Table 3); FLOP counting is not implemented.
- Hardware counters are opened per OS thread, lazily, the first time
  each thread needs them — correct for OpenMP+MPI hybrid codes as well
  as the single-threaded-per-rank case. (An earlier version opened one
  group on whichever thread called `MPI_Init` and read it from every
  thread's `SIGPROF` handler unconditionally; a sample landing in an
  OpenMP worker thread would report the `MPI_Init` thread's counters
  instead, collapsing every worker function's IPC/MPKI numbers
  toward whatever that one thread happened to be doing — usually idling
  at a barrier. Confirmed against a real MPI+OpenMP NAS Parallel
  Benchmark run: seven structurally unrelated kernels all showed
  near-identical `Cyc/samp`, which is the signature of measuring the
  same thread's counters every time. Fixed in `sampler_hwc.c`; see
  `make run-hwc-validate-omp` for a regression check that runs a
  deliberately compute-bound kernel and a deliberately memory-bound one
  concurrently on two OpenMP threads and asserts they're still
  classified correctly, independent of each other.)
- The `PROFILER` group's percentage (signal handler + dispatch overhead)
  scales inversely with `MINIPROF_INTERVAL_MS` — the default 10ms
  interval keeps this small for real workloads; the README's own
  example output used a deliberately short, fast-finishing run (a few
  seconds) with a tight 3ms interval to get a usable sample count
  quickly, which inflates the `PROFILER` percentage relative to what
  you'd see on a real multi-minute job. If `PROFILER` regularly exceeds
  a few percent in your own reports, increase `MINIPROF_INTERVAL_MS`.

## License

(add your license here)
