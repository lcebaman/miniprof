# miniprof

A lightweight parallel performance profiler for MPI + C/C++ on Linux,
inspired by Cray's CrayPat / perftools-lite, but vendor-neutral — it runs
on any Linux cluster with a standard MPI install, no Cray Programming
Environment required.

Two modes, mirroring CrayPat's own sampling → APA → tracing workflow:

- **sample** — statistical PC sampling via `SIGPROF`, near-zero overhead,
  safe to run on every job by default. Produces a CrayPat-style text
  report grouped by `Group → Function → Source Line`, plus a `.mpa`
  suggestions file listing the hot functions worth tracing in detail.
- **trace** — exact entry/exit instrumentation of the functions named in
  the `.mpa` file, plus per-call MPI timing (call count, time, message
  size). Higher overhead, used only for the functions that matter.

No GUI. Everything is plain text, designed to be read in a terminal or
piped into a job log.


## Requirements

- Linux, x86-64
- An MPI implementation with development headers (`mpicc`/`mpifort` on
  `$PATH`) — tested against Open MPI 4.1
- `addr2line`, `nm`, `objdump` (part of GNU binutils, present on every
  normal Linux box)
- GCC or Clang (for `-finstrument-functions` support in trace mode)
- gfortran, only if profiling Fortran MPI codes (see
  [Fortran support](#fortran-support))


## Building

```bash
make            # builds libminiprof.a + heat2d (sample-mode example)
```

That's the whole library plus a self-contained 2D heat-diffusion MPI
example used for testing. To also build the instrumented binary needed
for trace mode and the standalone validator:

```bash
make example-trace   # heat2d+trace — built with -finstrument-functions
make mpa-check        # standalone .mpa file validator, no MPI needed
```

Or build everything in one shot:

```bash
make mpa        # clean, build everything, run sampling, run tracing
```

### Using miniprof in your own MPI code

```bash
mpicc -O2 -g your_app.c -L/path/to/miniprof -lminiprof -ldl -lm -o your_app
```

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
    -L/path/to/miniprof -lminiprof -ldl -lm -o your_app+trace
```


## Fortran support

Fortran MPI codes work the same way, automatically — no manual init
calls, no separate API. Link your Fortran program against
`libminiprof.a` using `mpifort`:

```bash
mpifort -O0 -g examples/heat1d.f90 -L. -lminiprof -ldl -lm -o heat1d
mpirun -n 4 ./heat1d
```

For trace mode, build a second instrumented binary exactly as with C:

```bash
mpifort -O0 -g -finstrument-functions examples/heat1d.f90 \
    -L. -lminiprof -ldl -lm -o heat1d+trace
```

The Makefile has equivalent targets for the bundled Fortran example
(`examples/heat1d.f90`): `make example-f`, `make example-f-trace`,
`make run-f`, `make run-f-trace`.

### Why this needs separate support

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

(The Fortran init/finalize wrappers live in their own object file,
`miniprof_fortran.c`, rather than alongside the C ones — they reference
`pmpi_init_` etc., which only exist in `libmpi_mpifh.so`, a library
`mpicc` does not link by default. Keeping them separate means plain C
builds never need that dependency resolved.)

### `-O0` is required for the sampling/trace examples

gfortran inlines small internal `contains` subroutines very
aggressively — even at the default optimization level, and more
readily than C's `static` functions — which removes them as separate
symbols entirely, leaving nothing for the profiler to attribute time
to. Build Fortran code you want to sample or trace at `-O0` (or extract
the hot routines into separate module procedures, which inline less
eagerly than internal ones). This is purely a Fortran-frontend
behavior, unrelated to miniprof itself.

### What's verified working

Both modes were tested end to end against a real Fortran MPI program
(1D heat diffusion with internal `compute_step`/`exchange_halo`/
`check_norm` subroutines): sample mode correctly auto-starts via
`MPI_Init` interception and attributes time down to source line; trace
mode correctly resolves the gfortran-mangled subroutine names
(`compute_step.2`, etc.) via `mpa-check`, and Table 2 (MPI call timing)
is populated from the Fortran-side `MPI_Sendrecv`/`MPI_Allreduce` calls
exactly as it is for C, including correct exclusive-time exclusion of
MPI wait time.


## diffusion3d — the scientific benchmark example

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

./mpa-check diffusion3d+trace miniprof_3d.mpa  # validate before submitting
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


## Quick start: the two-pass workflow

This is the recommended way to use miniprof, and matches CrayPat's own
sampling → APA → tracing pattern.

### 1. Sample first

Run your normal binary with sampling enabled (the default mode — no env
vars required beyond pointing at an output file if you want a specific
name):

```bash
mpirun -n 64 ./your_app
```

This produces `miniprof_report.txt` — a CrayPat-style flat profile,
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

### 2. Validate before you trace

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

### 3. Trace

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


## Makefile reference

| Target | What it does |
|---|---|
| `make` / `make all` | Build `libminiprof.a` + `heat2d` (sample-mode example) |
| `make lib` | Build `libminiprof.a` only |
| `make example` | Build `heat2d` only (requires `lib`) |
| `make example-trace` | Build `heat2d+trace`, instrumented with `-finstrument-functions` |
| `make example-f` | Build `heat1d` (Fortran sample-mode example) |
| `make example-f-trace` | Build `heat1d+trace` (Fortran, instrumented) |
| `make mpa-check` | Build the standalone `.mpa` validator (no MPI dependency) |
| `make run` | Run `heat2d` in sample mode, 4 ranks; prints report and writes `miniprof.mpa` |
| `make run-trace` | Validate `miniprof.mpa` against `heat2d+trace`, then run trace mode, 4 ranks |
| `make run-f` | Fortran equivalent of `run` |
| `make run-f-trace` | Fortran equivalent of `run-trace` |
| `make example-3d` | Build `diffusion3d` (3D anisotropic diffusion, 2D Cartesian MPI) |
| `make example-3d-trace` | Build `diffusion3d+trace` (instrumented) |
| `make run-3d` | Sampling run on `diffusion3d`, 4 ranks |
| `make run-3d-trace` | Validate + trace run on `diffusion3d` |
| `make mpa` | Full two-pass workflow end to end: clean, build, sample, validate, trace |
| `make clean` | Remove all build artifacts and reports |

Override rank count: `make run NRANKS=16`.
Override the MPI compiler wrapper: `make CC=mpiicc` (Intel), etc.


## Environment variables

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


## Reading the sampling report

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

This mirrors CrayPat's `pat_report -O samp_profile+src` table layout:

- **Samp% / Samp** — fraction and absolute count of samples landing in
  this group/function/line, aggregated across all ranks.
- **Imb.Samp / Imb.Samp%** — load imbalance, computed the same way
  CrayPat does it: `Imb.Samp = max_rank_samples − mean_rank_samples`,
  `Imb.Samp% = Imb.Samp / max_rank_samples × 100`. A function with a
  large `Imb.Samp%` is unevenly distributed across ranks — a likely
  candidate for load-balancing work.
- **Group** — `USER` (your code), `MPI`, `OMP`, `LIBC`, `SYSTEM`
  (process startup), `PROFILER` (miniprof's own sampling overhead —
  see below), or `ETC` (anything unclassified).

Below the group/function table are a per-rank top-5 breakdown and the
raw per-rank sample counts and wall-clock times.


## Notes on symbol resolution

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


## Known limitations

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
- Hardware performance counters (PAPI-style FLOP/cache-miss counting,
  CrayPat's `PAT_RT_PERFCTR`) are not implemented.
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
