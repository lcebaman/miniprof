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
add the matching bindings, converting Fortran's by-reference `INTEGER`
handles to real MPI C handles via the standard `MPI_*_f2c()` /
`MPI_*_c2f()` API and delegating to the existing C wrapper logic, so
there's no duplicated timing code per language.

(The Fortran init/finalize wrappers in `src/miniprof_fortran.c` reference
`pmpi_init_` etc., which only exist in `libmpi_mpifh.so` — a library
`mpicc` does not link by default, only `mpifort` does. Since miniprof is
a shared library, that's harmless: the symbols resolve lazily at load
time, so `pmpi_init_` only needs to be found when `mpi_init_` is actually
called, which only happens for Fortran applications. Plain C builds never
trigger the lookup. This is also why linking against `libminiprof.so`
needs `-Wl,--allow-shlib-undefined` — see
[Link miniprof into your application](#link-miniprof-into-your-application).)

#### Optimization note

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


### Configuration

All of these are read at `MPI_Init` time; set them in your job script or
`mpirun -x VAR` (or just `export` before `mpirun`, depending on your MPI
launcher's environment-passing convention).

| Variable | Mode | Default | Meaning |
|---|---|---|---|
| `MINIPROF_MODE` | both | `sample` | `sample` or `trace` |
| `MINIPROF_INTERVAL_MS` | sample | `10` | SIGPROF sampling interval, milliseconds |
| `MINIPROF_DEPTH` | sample | `8` | Call-stack depth captured per sample |
| `MINIPROF_OUTPUT` | sample | `miniprof_report.txt` | Sampling report path |
| `MINIPROF_MPA_OUTPUT` | sample | `miniprof.mpa` | Where to write the trace-candidate suggestions |
| `MINIPROF_MPA_THRESHOLD` | sample | `1.0` | Minimum Samp% for a USER function to be suggested |
| `MINIPROF_TRACE_FUNCS` | trace | `miniprof.mpa` | Which `.mpa`-format file to read the trace function list from |
| `MINIPROF_TRACE_OUTPUT` | trace | `miniprof_trace_report.txt` | Tracing report path |
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
second table classifying every function as compute-bound or
memory-bound:

```
Table 2: Hardware Counter Characterisation (per function)
------------------------------------------------------------------------------------
      Samp     IPC  LLC miss/Ksamp    Cyc/samp  Classification  Function
------------------------------------------------------------------------------------
       832    1.83              12   1.140e+06  COMPUTE-BOUND   compute_viscous_dissipation
       659    0.42             847   4.220e+06  MEMORY-BOUND    compute_stencil
------------------------------------------------------------------------------------
  Classification: IPC>=1.2 = COMPUTE-BOUND, IPC<=0.5 + LLC miss/Ksamp>=500 = MEMORY-BOUND, else MIXED
```

- **IPC** — instructions retired per cycle for that function, summed
  across all ranks and all samples. High IPC means the pipeline is
  staying full; low IPC means the CPU is stalled, usually waiting on
  memory.
- **LLC miss/Ksamp** — last-level-cache misses per 1000 samples
  attributed to that function.
- **Classification** — a heuristic from IPC and LLC miss rate; the
  three cutoffs are architecture-dependent and configurable via
  `MINIPROF_IPC_COMPUTE_THRESHOLD`, `MINIPROF_IPC_MEMORY_THRESHOLD`, and
  `MINIPROF_LLC_MEMORY_THRESHOLD` (defaults: 1.2, 0.5, 500 — tune these
  per CPU generation; the "right" cache-miss rate to call memory-bound
  varies a lot by memory subsystem).

The counter deltas between consecutive samples are attributed to
whichever leaf PC the *second* sample landed on — an approximation, not
exact per-instruction accounting, but one that converges to a
representative IPC/miss-rate given enough samples for a function, the
same statistical argument that makes PC sampling itself valid.

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

**PIE binaries need load-base correction.** Every modern Linux binary is
position-independent by default, so the addresses `nm`/`addr2line` print
(link-time addresses, relative to the binary's own coordinate system)
don't match the addresses you see at runtime (`backtrace()`,
`__cyg_profile_func_enter`'s `this_fn` argument) until you account for
where the loader actually placed the binary in memory. miniprof reads its
own load base from `/proc/self/maps` (`dli_fbase` via `dladdr` for
sampling; a `/proc/self/maps` scan for trace mode) and applies the offset
before every comparison.

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

- `MPI_Wait`/`MPI_Test` family and one-sided RMA calls beyond the basics
  wrapped in `mpi_trace.c` (`Send`, `Recv`, `Sendrecv`, `Isend`, `Irecv`,
  `Wait`, `Barrier`, `Allreduce`, `Reduce`, `Bcast`, `Gather`,
  `Allgather`) are not individually timed — anything not explicitly
  wrapped falls through to the real MPI call with zero overhead and
  zero report entry. Extending coverage is mechanical; see the pattern
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
  cache-misses (see [Table 2](#table-2-hardware-counter-characterisation));
  FLOP counting is not implemented.
- The hardware counter group (Table 2) is opened by, and only counts,
  the thread that calls `MPI_Init` — correct for the common
  single-threaded-per-rank case, but not extended to per-OpenMP-thread
  counter groups yet. If `SIGPROF` fires on a different thread in an
  OpenMP+MPI hybrid code, the counter delta gets attributed to the
  wrong thread's activity; sample counts (Table 1) are unaffected, only
  Table 2's IPC/LLC-miss numbers would be misleading in that scenario.
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
