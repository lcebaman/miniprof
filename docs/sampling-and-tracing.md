# Hotspots, sampling and tracing

A snapshot tells you *where the time goes by category* (compute, MPI, I/O,
OpenMP regions). To see *which functions*, miniprof has three tools, from
cheapest to most precise:

| Tool | How | Gives | Rebuild? |
|---|---|---|---|
| `miniprof --hotspots` | CPU-time sampling alongside a snapshot | top 30 functions by exclusive CPU time, per-rank imbalance, in the snapshot report | no |
| `miniprof --sample` | call-stack sampling (sample mode) | every function and source line, by group, per rank; a list of functions worth tracing | no (`-g` for lines) |
| trace mode | compiler instrumentation of chosen functions | exact calls, inclusive and exclusive time, counters per call; every MPI call | yes |

Hotspots are described in [snapshot reports](snapshot.md#hotspots). This
page covers sampling and tracing, which together form a two-pass
workflow: **sample broadly, select the functions that matter, trace those
precisely.**

- [1. Sample](#1-sample)
- [2. Select and validate](#2-select-and-validate)
- [3. Trace](#3-trace)
- [Reading the sampling report](#reading-the-sampling-report)
- [Hardware counters per function](#hardware-counters-per-function)
- [Symbol resolution notes](#symbol-resolution-notes)

## 1. Sample

```bash
mpirun -np 64 miniprof --sample ./app
```

A real run of the bundled `diffusion3d` on 4 ranks:

```
miniprof: top routines: compute_diffusion 18%, compute_viscous_dissipation 18%, compute_darcy_advection 15%, compute_radiation 9%, apply_bc 8%
miniprof: profile /home/user/miniprof_diffusion3d_20261116_185557/miniprof_report.txt
```

Each rank's program counter and call stack are sampled every 10 ms
(`SIGPROF`; `MINIPROF_INTERVAL_MS`). The result folder holds:

- `miniprof_report.txt`: the profile (below);
- `miniprof.mpa`: every application function above 1% of samples
  (`MINIPROF_MPA_THRESHOLD`), the candidates for tracing.

Without the launcher: `LD_PRELOAD=.../libminiprof.so` (sample is the
default `MINIPROF_MODE`), writing `miniprof_report.txt` and `miniprof.mpa`
in the working directory.

## 2. Select and validate

`miniprof.mpa` is plain text: one function per line, `#` starts a
comment. Edit it freely: add functions you care about, remove ones you
don't.

```
# Function                             Samp%
compute_diffusion                   #  18.0%
compute_viscous_dissipation         #  17.6%
compute_darcy_advection             #  15.0%
compute_radiation                   #   9.2%
apply_bc                            #   8.5%
```

MPI calls are never listed: they are always traced.

Trace mode needs a second build of the application, instrumented:

```bash
mpicc -O2 -g -finstrument-functions app.c -L$MINIPROF_HOME/lib -lminiprof \
    -Wl,-rpath,$MINIPROF_HOME/lib -Wl,--allow-shlib-undefined -ldl -lm -o app+trace
```

Before spending a large allocation on it, check that every listed
function resolves in that binary, and that it really was built with
`-finstrument-functions`:

For the bundled `heat2d`:

```
$ mpa-check app+trace miniprof.mpa
mpa-check: validating 2 function(s) from 'miniprof.mpa' against 'app+trace'

  OK    compute_stencil                           0x00003be0  (local/static)
  OK    check_convergence                         0x00003e50  (local/static)

2 resolved, 0 failed, out of 2 total

mpa-check: confirmed 4 instrumentation call site(s) in 'app+trace' — this binary was built with -finstrument-functions.

mpa-check: all functions resolved successfully.
```

It exits non-zero on any failure, so it can gate a job script
(`mpa-check app+trace miniprof.mpa || exit 1`). A failure says why: a
typo, a function inlined away (try `__attribute__((noinline))` or `-O0`
for that file), an `.mpa` file from another binary, or a stripped binary.
`mpa-check` is built with `make mpa-check` and needs no MPI.

## 3. Trace

```bash
MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=miniprof.mpa mpirun -np 64 ./app+trace
```

`app+trace` links the library (the instrumentation hooks live in it), so
no preload is needed. It writes `miniprof_trace_report.txt` (here from the
bundled `heat2d`):

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

Exclusive time excludes time inside MPI calls: `check_convergence`'s
exclusive time (7.28 s) is 4.58 s below its inclusive time, exactly the
`MPI_Allreduce` time it waits in.

With hardware counters, a third table gives exact per-call counters,
read at every traced function's entry and exit:

```
Table 3: Hardware Counter Characterisation (exact per-invocation, inclusive)
   Calls    IPC    MPKI      Cycles  Instructions  LLC misses  Classification  Function
    1600   3.68    4.47   1.095e+10     4.028e+10   1.799e+08  COMPUTE-BOUND   compute_stencil
    1600   1.51   13.28   8.997e+09     1.356e+10   1.802e+08  COMPUTE-BOUND   check_convergence
```

These are inclusive (the whole call, including callees) and exact, so
they are the more precise numbers when they disagree with the sampling
report's estimate.

## Reading the sampling report

```
Table 1: Profile by Group, Function, and Line
   Samp%      Samp   Imb.Samp  Imb.Samp%  Group
                                           Function
                                            Source Line
----------------------------------------------------------------------------------------
  100.0%      4089         --         --  Total
   91.8%      3755         --         --  USER
   18.0%       734        9.5       4.9%   compute_diffusion
    5.2%       214         --         --    diffusion3d.c:521
    2.5%       102         --         --    diffusion3d.c:518
   17.6%       720        8.0       4.3%   compute_viscous_dissipation
    6.3%       258         --         --    diffusion3d.c:668
```

- **Samp% / Samp**: share and number of samples, across all ranks.
- **Imb.Samp / Imb.Samp%**: load imbalance across ranks,
  `max − mean` samples and that as a % of the max. A large value marks a
  function unevenly spread across ranks.
- **Group**: `USER` (your code), `MPI`, `OMP`, `LIBC`, `SYSTEM` (process
  start-up), `PROFILER` (miniprof's own signal handler), `ETC`.

Below the table: the top 5 functions per rank, and samples and wall time
per rank. If `PROFILER` exceeds a few percent, raise
`MINIPROF_INTERVAL_MS`.

## Hardware counters per function

If counters are available, each sample also accumulates cycles,
instructions and last-level-cache misses, and the sampling report gains a
table classifying each application function:

```
Table 2: Hardware Counter Characterisation (per function)
      Samp     IPC    MPKI      Cycles  Instructions  LLC misses  Classification  Function
       832    1.83    0.09   9.480e+08     1.734e+09   1.561e+05  COMPUTE-BOUND   compute_viscous_dissipation
       659    0.42   28.14   2.782e+09     1.168e+09   3.288e+07  MEMORY-BOUND    compute_stencil
  Classification: IPC>=1.2 = COMPUTE-BOUND, IPC<=0.5 + MPKI>=10.0 = MEMORY-BOUND, else MIXED
```

- **MPKI**: last-level-cache misses per 1000 instructions.
- The cut-offs depend on the CPU; set them with
  `MINIPROF_IPC_COMPUTE_THRESHOLD`, `MINIPROF_IPC_MEMORY_THRESHOLD` and
  `MINIPROF_MPKI_MEMORY_THRESHOLD`.
- The counter change between two samples is credited to the function the
  second sample lands in. That converges for functions that run longer
  than one sampling interval. For many short functions, one interval spans
  many of them and the numbers reflect the interval, not the function
  ("interval smearing"). The report **detects this and warns**: when most
  functions show nearly the same cycles per sample and the same
  classification. Use trace mode (Table 3) for exact per-function numbers
  then.

## Symbol resolution notes

- `static` functions and Fortran internal procedures are not in the
  dynamic symbol table, so `dladdr()` cannot name them, with or without
  `-rdynamic`. miniprof reads the binary's regular symbol table instead
  (in-process for hotspots, `addr2line`/`nm` for sampling and tracing).
  `-rdynamic` is not needed.
- `addr2line` gives source lines and inlined-function names when the binary
  has `-g`. When GNU `addr2line` cannot read a DWARF 5 line table (binutils
  2.35 on RHEL 9 with AOCC or clang binaries), miniprof retries with
  `llvm-addr2line`, `llvm-symbolizer` or `eu-addr2line` if one is on
  `PATH`.
- PIE and non-PIE executables are handled differently (load-base
  correction only for PIE); `mpa-check` checks names, the run checks
  addresses.
- A `??` with high weight, or a function that resolves in `mpa-check` but
  never appears, are the first things to report.
