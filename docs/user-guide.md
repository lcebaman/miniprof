# User guide

- [Running with the launcher](#running-with-the-launcher)
- [Launcher options](#launcher-options) ([each option in detail](#each-option-in-detail))
- [What you get](#what-you-get)
- [Choosing a level](#choosing-a-level)
- [Launch recipes](#launch-recipes)
- [Without the launcher](#without-the-launcher)
- [Regions: timing parts of your code](#regions-timing-parts-of-your-code)
- [Linking miniprof in](#linking-miniprof-in)
- [Fortran](#fortran)
- [Troubleshooting](#troubleshooting)

## Running with the launcher

Put `miniprof` in front of the executable, after `mpirun`/`srun` and any
binding wrapper, and keep the rest of your command as it is:

```bash
module load miniprof
mpirun -np 96 --map-by ppr:48:socket:pe=2 --bind-to core miniprof pw.x -in scf.in
srun -n 96 --cpu-bind=cores miniprof pw.x -in scf.in
mpirun -np 96 ./numa_bind.sh miniprof pw.x -in scf.in       # after your wrapper
```

Nothing is recompiled. The launcher runs on every rank and starts the
application with `libminiprof.so` preloaded into it alone, not into
`mpirun` or your wrapper. Because it sets its variables itself, nothing
has to be forwarded with `mpirun -x`, and it works the same under
`mpirun` and `srun`. Input redirected into the job (`pw.x < scf.in`)
still reaches the application on rank 0.

When the run ends, rank 0 writes the reports and prints a summary:

```
miniprof: lmp, 48 ranks x 4 threads on 1 node(s), 9.8 s: compute 82%, MPI 18% (MPI_Send 7%), I/O 0%
miniprof: report /home/user/mp-test/miniprof_lmp_20261113_122838/report.html
```

The application's exit code is passed through. An application miniprof
cannot attach to (statically linked, or built with a different MPI) runs
unprofiled, with a one-line message. A run that dies before
`MPI_Finalize` writes no snapshot.

## Launcher options

Options go between `miniprof` and the application (each is explained [below](#each-option-in-detail)):

| Option | Meaning |
|---|---|
| `-l`, `--level L` | snapshot detail: `basic` (default), `topdown`, `roofline` or `full`, see [below](#choosing-a-level) |
| `-H`, `--hotspots` | add the top functions to the snapshot (CPU-time sampling every 10 ms per thread); off by default |
| `-s`, `--sample` | a sampling profile instead of a snapshot: time per function and source line, per rank |
| `-o`, `--output DIR` | result folder (default `miniprof_<app>_<date>_<time>` in the working directory) |
| `--no-report` | write only `snapshot.json` (no text, HTML or CSV) |
| `-q`, `--quiet` | no summary lines |
| `--check` | check whether the application can be profiled on this node, then exit |
| `-h`, `--help` | usage |

```bash
mpirun -np 96 miniprof --level topdown -o td_run pw.x -in scf.in
mpirun -np 96 miniprof --hotspots pw.x -in scf.in
mpirun -np 96 miniprof --sample pw.x -in scf.in
miniprof --check $(which pw.x)
```

`MINIPROF_LEVEL` sets the default level. Every `MINIPROF_*` variable in
[configuration](configuration.md) still applies.

### Each option in detail

**`-l`, `--level L`** chooses which hardware counters the run collects.
Everything that is not a CPU counter (time split, MPI, I/O, OpenMP,
memory bandwidth and power, hotspots) is collected at every level; the
level only decides the counter group, because a CPU has too few counters
to collect them all in one run:

- `basic`: IPC, GFLOP/s and DRAM fills; the first look at any code.
- `topdown`: where the pipeline slots go (frontend, bad speculation,
  backend memory or core, retiring); use it when compute dominates and IPC
  is low, to see *why*.
- `roofline`: FLOPs and DRAM reads including prefetches, so each
  [region](#regions-timing-parts-of-your-code) gets its own roofline point.
- `full`: FP operations by vector width, to see how much code is
  vectorised.

Each level is a separate run; IPC from a `basic` run and top-down from a
`topdown` run of the same input can be read together. Hardware detail
beyond IPC needs AMD Zen 4, 5 or 6 ([hardware support](hardware.md)); the
roofline and the fingerprint's DRAM and FLOP measures also need a
calibration ([installing](installing.md)). See also
[choosing a level](#choosing-a-level).

**`-H`, `--hotspots`** adds the functions that use the CPU time to the
snapshot. Every thread is interrupted every 10 ms of its CPU time and the
interrupted function is counted; at the end each rank names its functions
and the report lists the top 30 across ranks, with CPU seconds per rank
and imbalance. It appears as a *Hotspots* section in `report.txt` and
`report.html`, `csv/hotspots.csv`, and a `hotspots:` line in the
summary. It is off by default because it adds a timer signal per thread
and some work at `MPI_Finalize` (naming functions, and reading debug
information to name inlined ones). Use it when the snapshot says compute
dominates but not *where*, typically in MPI-only codes without OpenMP
regions. Build with `-g` (optimisation unchanged) so inlined functions get
their own names; otherwise their time appears under the caller.
`MINIPROF_HOTSPOTS_INTERVAL_MS` and `MINIPROF_HOTSPOTS_INLINE` tune it
([configuration](configuration.md)); [snapshot reports](snapshot.md#hotspots)
explains how to read the table.

**`-s`, `--sample`** runs a sampling profile *instead of* a snapshot: call
stacks every 10 ms, reported per function and per source line, grouped
(USER, MPI, LIBC, ...) and per rank. The folder holds
`miniprof_report.txt` (the profile), `miniprof.mpa` (the functions worth
tracing) and `run.txt`; there is no `snapshot.json` or HTML report, and
the summary line lists the top routines. Use it when you need more than
`--hotspots` gives: inclusive call paths, source lines, or the list of
functions to trace precisely. See [hotspots, sampling and
tracing](sampling-and-tracing.md).

**`-o`, `--output DIR`** names the result folder. Rank 0 creates it
(parents included) relative to its working directory; the default is
`miniprof_<app>_<date>_<time>`, so repeated runs do not overwrite each
other. An existing folder is reused and its files replaced. If the
application stops before `MPI_Finalize`, an empty folder is removed.

**`--no-report`** writes only `snapshot.json` and `run.txt`: no text, HTML
or CSV reports. Use it when the compute nodes have no Python 3, or to keep
the end of a large job short; make the reports later with
`miniprof-report snapshot.json --html report.html -o report.txt --csv csv`.

**`-q`, `--quiet`** drops the summary lines rank 0 prints at the end. The
reports are still written. Useful in sweeps and scripts.

**`--check`** looks at the application and the node without running
anything, and exits: no `mpirun` needed, so run it on a login node or at
the start of a job script. It prints the miniprof installation, the MPI
library the application uses, its OpenMP runtime (and warns when it is
GCC's `libgomp`, which has no OpenMP detail), `perf_event_paranoid` (2 or
less for hardware counters), whether the memory helper can read the
memory controllers, and whether a calibration is set:

```
$ miniprof --check $(which pw.x)
miniprof 1.1.0 in /opt/miniprof/1.1
application: /opt/qe/bin/pw.x
MPI: /opt/openmpi/5.0/lib/libmpi.so.40
OpenMP: libomp.so
perf_event_paranoid: 2
memory helper: ready: /opt/miniprof/1.1/lib/miniprof-nodemon has CAP_PERFMON
calibration: none in (MINIPROF_CALIBRATION unset): the roofline needs one: miniprof-calibrate -o DIR on one node of each type
```

It exits with 1 and a reason (`cannot profile: ...`) when miniprof cannot
attach: the application is statically linked, not found, or uses a
different MPI library from the one this miniprof was built for. Without
`--check`, such an application simply runs unprofiled with the same
one-line reason.

**`--`** ends the options, for an application whose name starts with `-`.

**`-h`, `--help`** prints the options.

## What you get

A snapshot run writes:

| File | Contents |
|---|---|
| `report.html` | the full report, self-contained: one file, no external fonts or scripts, opens offline, can be mailed. Every chart has its numbers in a table; hovering shows values; follows light/dark mode |
| `report.txt` | the same as a one-page text report |
| `snapshot.json` | everything measured, in a versioned JSON document; every report is generated from it |
| `csv/` | the tables as CSV (summary, ranks, MPI functions, message sizes, I/O, regions, OpenMP, memory, roofline, hotspots) |
| `run.txt` | command, level, exit code, date, host |

A sampling run (`--sample`) writes `miniprof_report.txt` (the profile),
`miniprof.mpa` (functions worth tracing) and `run.txt`; see
[sampling and tracing](sampling-and-tracing.md).

Reports can be regenerated from `snapshot.json` at any time, for example
after updating miniprof or with different thresholds:

```bash
miniprof-report snapshot.json --html report.html -o report.txt --csv csv
miniprof-report snapshot.json --oneline
```

Every report includes a [fingerprint](snapshot.md#fingerprint): a one-line
class ("compute 63% memory-latency bound; MPI 12% bandwidth (large
messages)") and a fixed set of percentages that place the run against the
machine. To compare several runs at once:

```bash
miniprof-fingerprint run1 run2 run3 --html fingerprints.html
```

[Snapshot reports](snapshot.md) explains every section.

## Choosing a level

The CPU has only a few programmable counters per thread, so each run
collects one counter group. Everything that is not a CPU counter (time
split, MPI, I/O, OpenMP, memory, hotspots) is collected at every level.

| Level | Counters | Use it for |
|---|---|---|
| `basic` | cycles, instructions, FLOPs, DRAM fills | the first look: IPC, GFLOP/s, roofline per node |
| `topdown` | pipeline slots: frontend, bad speculation, backend (memory/core), retiring | *why* the core is slow (AMD Zen 4/5/6) |
| `roofline` | cycles, FLOPs, DRAM reads incl. prefetch | roofline points per user region |
| `full` | FP operations by vector width | how much code is vectorised |

A typical sequence: `basic` for the picture; `topdown` when compute
dominates and IPC is low; `roofline` with [regions](#regions-timing-parts-of-your-code)
to place individual kernels; `--hotspots` or `--sample` to find which
functions to look at.

## Launch recipes

**Hybrid MPI + OpenMP.** Give each rank as many cores as threads and bind:

```bash
export OMP_NUM_THREADS=4 OMP_PLACES=cores OMP_PROC_BIND=close
mpirun -np 48 --map-by ppr:24:socket:pe=4 --bind-to core miniprof ./app
srun -n 48 -c 4 --cpu-bind=cores miniprof ./app
```

The report checks this: ranks allowed to run on far more CPUs than they use
are flagged ("CPU binding: none"), because unbound ranks drift between
sockets and read memory remotely.

**A batch job.** The module path must be set inside the job script:

```bash
#!/bin/bash
#SBATCH -N 2 --exclusive -t 01:00:00
module load aocc openmpi
module use $HOME/sw/miniprof/1.1/share/modulefiles
module load miniprof/1.1
cd $SLURM_SUBMIT_DIR || exit 1
mpirun -np 192 --map-by ppr:48:socket:pe=2 --bind-to core miniprof ./app -in input
```

**Measuring the cost.** Run the same case with and without miniprof and
compare the application's own timer. Alternate the runs
(`off, profiled, off, profiled, off`) so that a node drifting over time is
not mistaken for overhead.

## Without the launcher

The launcher only sets environment variables. The equivalent by hand:

```bash
mpirun -np 64 env LD_PRELOAD=$MINIPROF_HOME/lib/libminiprof.so \
    MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=basic \
    MINIPROF_SNAPSHOT_OUTPUT=$PWD/snapshot.json ./app
miniprof-report snapshot.json --html report.html
```

Use `env ... ./app` after `mpirun` rather than exporting `LD_PRELOAD`
before it, so that only the application loads the library. With Open MPI,
variables exported before `mpirun` reach remote ranks only if named with
`-x`. miniprof decides its collective steps at `MPI_Finalize` jointly
across ranks, so a variable that reaches only some ranks does not hang
the run, but the result is incomplete.

## Regions: timing parts of your code

To get time and counters per phase of your code (and roofline points per
kernel at the `roofline` level), bracket the code:

```c
#include "miniprof.h"            /* -I$MINIPROF_HOME/include */

miniprof_region_begin("solver");
solve(...);
miniprof_region_end("solver");
```

```fortran
call MINIPROF_REGION_BEGIN('solver')
call solve(...)
call MINIPROF_REGION_END('solver')
```

Link with `-L$MINIPROF_HOME/lib -lminiprof -Wl,-rpath,$MINIPROF_HOME/lib
-Wl,--allow-shlib-undefined` (see [linking](#linking-miniprof-in)).
Regions are inclusive and nest (up to 16 deep per thread, 64 distinct
names); they may differ between ranks and are reduced across ranks by
name. Outside snapshot mode they cost a function call and do nothing, so
they can stay in production code. A region entered only on the master
thread of a hybrid code measures the master thread's work.

## Linking miniprof in

You can link the library instead of preloading it, so the binary always
runs under miniprof (`MINIPROF_MODE=off` turns it off):

```bash
mpicc -O2 -g app.c -L$MINIPROF_HOME/lib -lminiprof \
    -Wl,-rpath,$MINIPROF_HOME/lib -Wl,--allow-shlib-undefined -ldl -lm -o app
```

`--allow-shlib-undefined` is needed because the library references the
Fortran PMPI symbols (`pmpi_init_` ...), which resolve only in Fortran
programs; C programs never call them. Trace mode needs a second build with
`-finstrument-functions`, see [sampling and tracing](sampling-and-tracing.md#3-trace).

`-g` is not required for any mode, but gives source file and line in
sampling reports and OpenMP region names. `-rdynamic` is not needed.

## Fortran

Fortran programs work the same way, with no source changes. miniprof
intercepts the Fortran MPI bindings (`mpi_init_`, `mpi_allreduce_`, ...)
as well as the C ones, and forwards each call, arguments untouched, to
the MPI library's own Fortran PMPI binding, so `MPI_IN_PLACE`,
`MPI_STATUS_IGNORE` and the other sentinels keep their meaning
(`make run-fortran-sentinels` checks every wrapped call). The `mpi`
module and `mpif.h` are covered; the `mpi_f08` module is not yet (see
[limitations](limitations.md)).

Compilers may inline small internal (`contains`) procedures, which then
have no symbol of their own; their time shows under the caller.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `miniprof: command not found` in a job | the module was loaded in your login shell only: add `module use` / `module load` to the job script |
| `not profiling: ... uses libmpi.so.X but this miniprof was built for libmpi.so.Y` | load the miniprof module built for that MPI |
| `hardware counters unavailable: ... Permission denied` | `perf_event_paranoid` is 3 or more; ask for 2 |
| `memory block unavailable` | grant the helper its capability ([installing](installing.md#the-memory-helper)), or not a Zen 4/5/6 node |
| `roofline: no calibration` | run `miniprof-calibrate` for this node type |
| `OpenMP not measured: no OMPT` | GCC's `libgomp`; build with clang, AOCC or Intel oneAPI for the OpenMP section |
| `OpenMP not measured: ... ran no parallel region` | the application links an OpenMP runtime but did not use it |
| routines missing in an MPI-only code | the snapshot names OpenMP regions only: add `--hotspots`, or use `--sample` |
| `CPU binding: none` | ranks are not bound; add `--bind-to core` (with `--map-by ...:pe=N` for N threads) or `srun --cpu-bind=cores` |
| no `report.html`, only `snapshot.json` | Python 3 missing on the node, or see `report.err` in the result folder; regenerate with `miniprof-report` |
