# Examples

Worked examples, from a first run to comparing machines. The numbers are
from real runs: the bundled `diffusion3d` benchmark, LAMMPS (molecular
dynamics, MPI + OpenMP) and MOM6 (ocean model, MPI only) on two-socket
AMD EPYC 9654 nodes (Genoa, 192 cores), Open MPI 5 and AOCC.

1. [A first run](#1-a-first-run)
2. [A hybrid MPI + OpenMP code (LAMMPS)](#2-a-hybrid-mpi--openmp-code-lammps)
3. [An MPI-only code with load imbalance (MOM6)](#3-an-mpi-only-code-with-load-imbalance-mom6)
4. [Why is one node pair slower?](#4-why-is-one-node-pair-slower)
5. [Choosing ranks × threads](#5-choosing-ranks--threads)
6. [Timing phases of your code with regions](#6-timing-phases-of-your-code-with-regions)
7. [Measuring miniprof's own cost](#7-measuring-miniprofs-own-cost)
8. [Characterising a set of applications](#8-characterising-a-set-of-applications)

## 1. A first run

The repository's `diffusion3d` solves a 3D diffusion problem with a 2D MPI
decomposition and halo exchanges.

```bash
make examples
mpirun -np 4 miniprof ./examples/diffusion3d
```
```
miniprof: diffusion3d, 4 ranks on 1 node(s), 20.5 s: compute 93%, MPI 7% (MPI_Sendrecv 7%), I/O 0%
miniprof: report /home/user/miniprof_diffusion3d_20261116_185507/report.html
```

Open `report.html`. This one is simple: 93% compute, balanced across
ranks (imbalance 0.8%), and MPI is the halo exchange, 128 KiB
`MPI_Sendrecv` messages. To see which functions the compute is in:

```bash
mpirun -np 4 miniprof --sample ./examples/diffusion3d
```
```
miniprof: top routines: compute_diffusion 18%, compute_viscous_dissipation 18%, compute_darcy_advection 15%, compute_radiation 9%, apply_bc 8%
```

`miniprof --hotspots` puts the same functions in the snapshot report
instead. Both need `-g` to name these kernels: at `-O2` the compiler
inlines them into `main`, and only the debug information still knows
them. The sampling profile adds source lines.

## 2. A hybrid MPI + OpenMP code (LAMMPS)

LAMMPS's rhodopsin benchmark (`bench/in.rhodo.scaled`, replicated to 2
million atoms), 48 ranks × 4 OpenMP threads on one node:

```bash
export OMP_NUM_THREADS=4 OMP_PLACES=cores OMP_PROC_BIND=close
miniprof --check $(which lmp)
mpirun -np 48 --map-by ppr:24:socket:pe=4 --bind-to core \
    miniprof lmp -in in.rhodo.scaled -sf omp -pk omp 4 -var x 4 -var y 4 -var z 4
```

Then the same run at `--level topdown` and `--level roofline`.

**Is it trustworthy?** Compared with unprofiled runs alternating with the
profiled ones (1,000 steps, about 100 s each), the wall time changed by
+3.1% (`basic`), −1.0% (`topdown`) and +0.2% (`roofline`), against
3.9% spread between the unprofiled runs themselves; LAMMPS's own loop
timer, +2.6%, −1.3%, −0.1%. Energies and trajectory files were identical
to the unprofiled run's, and miniprof's compute and MPI times matched
LAMMPS's own timing breakdown.

**What the report showed:**

- **MPI is about 20% of the run, and LAMMPS's "Comm" is only a small part
  of it.** Most of it is inside the long-range solver (PPPM), whose 3D
  FFTs exchange data between ranks: `MPI_Send`, `MPI_Waitany` and `MPI_Wait`
  lead the MPI table. Fewer ranks with more threads each means fewer,
  larger FFT exchanges (see [example 5](#5-choosing-ranks--threads)).
- **The core, not memory, limits it.** Roofline: 316 GFLOP/s at 2.6 FLOPs
  per byte, 120 GB/s of DRAM traffic (16% of the node's 768 GB/s): below
  both roofs. Top-down: 20% of slots lost to bad speculation and 45%
  backend bound, typical of the branchy neighbour-list loops in MD codes.
- **OpenMP:** in the short 100-step run, 30.6% of the time was outside
  parallel regions, but after subtracting MPI only 13.0% was serial
  compute. Worker threads spun between regions for 21% of all cycles
  (7.8 core-seconds per rank), which the report flags; with that spin kept
  out, the compute IPC was 1.84 against 1.61 overall.

**On two nodes** (96 × 4), the loop took 42.4 s instead of 95.6 s, MPI fell
to 12% (smaller FFT exchanges per rank), and the profiled runs were within
+0.9% and +0.6% of the unprofiled ones.

## 3. An MPI-only code with load imbalance (MOM6)

MOM6, 192 MPI ranks on one node, launched without binding:

```bash
mpirun -np 192 miniprof ./MOM6
```

The report's observations:

- **Half the run is MPI** (48.9%), almost all of it `MPI_Wait`: 1.7
  billion calls, about 9 million per rank.
- **Most of that is waiting for slower ranks.** Compute imbalance is 34.7%:
  the slowest rank computes for 111 s against an average of 72.5 s. MOM6
  decomposes the domain by area, and domains with more land do less work.
  A land mask (dropping all-land domains) or another processor layout is
  the usual remedy.
- **CPU binding: none.** Each rank was allowed all 384 CPUs and kept about
  one busy. 13% of its memory reads came from the other socket, against
  about 4% in the bound LAMMPS runs, because the OS moves unbound ranks
  between sockets. The fix is free:

  ```bash
  mpirun -np 192 --bind-to core miniprof ./MOM6
  ```

- **No routines in the report:** MOM6 here uses no OpenMP, and the
  snapshot only names OpenMP regions. To see the routines:

  ```bash
  mpirun -np 192 --bind-to core miniprof --hotspots ./MOM6    # top functions in the snapshot
  mpirun -np 192 --bind-to core miniprof --sample ./MOM6      # full profile, source lines with -g
  ```

  With half the run in MPI, expect Open MPI's internals (`opal_progress`,
  `[mca_btl_vader.so]`) near the top, grouped under MPI.

## 4. Why is one node pair slower?

The same 2-node LAMMPS run, twice, on two different pairs of nodes. One
took 48 s, the other 80 s. `miniprof-diff` on the two `basic` snapshots
(abridged):

```bash
miniprof-diff pairA/snapshot.json pairB/snapshot.json --label-a "pair A" --label-b "pair B" --html pairs.html
```
```
 * B is 1.73x slower than A (43.95 s -> 76.05 s, slowest rank).
 * Per average rank the run lost 32.11 s; MPI accounts for 32.25 s of it (100% of the change).
 * Largest MPI change: MPI_Waitany, 1.21 s -> 24.42 s per rank (+23.20 s).
 * Energy to solution (package, all nodes): 70.7 kJ -> 117.4 kJ (x1.66) at 805 W -> 772 W.

 Where the time went (average rank)      A            B          change   share
   compute                       38.56 s      38.42 s     -0.14 s     -0%
   MPI                            5.38 s      37.63 s    +32.25 s    100%

 MPI functions (time per rank)    A          B     change    calls A -> B
   MPI_Waitany                 1.214     24.416    +23.202    12,518,506 -> 12,518,506
   MPI_Wait                    1.352      9.016     +7.664    2,958,045 -> 2,958,045
   MPI_Allreduce               0.785      1.812     +1.027    693,120 -> 693,120
```

Compute is unchanged, and the extra 32 s are waits on **the same messages**
(identical call counts), with no new imbalance: each message takes longer
to arrive. That points at the network between those two nodes (transport
selection, a degraded link, or placement on the fabric), not at the
application or the CPUs. A later run was back to 47 s, and
the energy cost of the slow pair was ×1.66.

## 5. Choosing ranks × threads

Keep ranks × threads equal to the cores, and bind each rank to its
threads' cores. A sweep on one 192-core node, one report per point
(LAMMPS's `-pk omp 0` takes `OMP_NUM_THREADS`):

```bash
export OMP_PLACES=cores OMP_PROC_BIND=close
for t in 1 2 4 8 16; do
    n=$((192 / t)); export OMP_NUM_THREADS=$t
    mpirun -np $n --map-by ppr:$((n / 2)):socket:pe=$t --bind-to core \
        miniprof -o sweep_${n}x${t} lmp -in in.rhodo.scaled -sf omp -pk omp 0 \
        -var x 4 -var y 4 -var z 4
done
miniprof-diff sweep_48x4/snapshot.json sweep_24x8/snapshot.json --html 48x4_vs_24x8.html
miniprof-fingerprint sweep_192x1 sweep_96x2 sweep_48x4 sweep_24x8 sweep_12x16 --html sweep.html
```

The fingerprint table shows every point of the sweep in one view: the MPI,
OpenMP serial and barrier-wait rows move against each other as threads
per rank go up.

What to watch as threads per rank go up:

- **MPI share** falls (fewer ranks exchange fewer, larger messages);
- **serial compute** and **OpenMP worker idle** rise (code the threads do
  not share leaves more cores idle);
- the best point is where the MPI saved still outweighs the serial cost
  added; the diff between neighbouring points shows which wins.

## 6. Timing phases of your code with regions

To see time, counters and a roofline point per phase, bracket the phases
and run at `--level roofline`:

```c
#include "miniprof.h"

for (int step = 0; step < nsteps; step++) {
    miniprof_region_begin("stencil");
    compute_stencil(u, unew);
    miniprof_region_end("stencil");

    miniprof_region_begin("halo");
    exchange_halo(unew);
    miniprof_region_end("halo");
}
```

```bash
mpicc -O2 -g app.c -I$MINIPROF_HOME/include -L$MINIPROF_HOME/lib -lminiprof \
    -Wl,-rpath,$MINIPROF_HOME/lib -Wl,--allow-shlib-undefined -o app
mpirun -np 64 miniprof --level roofline ./app
```

The report then has a regions table and one roofline point per region,
FLOPs per DRAM byte read against the read-only bandwidth ceiling. A
streaming kernel such as `daxpy` sits at 0.125 FLOP/byte (2 FLOPs per 16
bytes read); the validation checks this on Zen 4, 5 and 6. The calls cost
a function call when not profiling, so they can stay in the code.

## 7. Measuring miniprof's own cost

The report's run table shows miniprof's start-up and shut-down time
(typically a few tens of milliseconds). For the cost during the run,
compare the application's own timer with and without miniprof, and
alternate the runs so a node that drifts over time is not mistaken for
overhead:

```bash
for i in 1 2 3; do
    mpirun -np 48 ./app > off_$i.log
    mpirun -np 48 miniprof -q -o prof_$i ./app > prof_$i.log
done
grep -h "Loop time" off_*.log prof_*.log        # or whatever your code prints
```

Differences smaller than the spread between the unprofiled runs are noise.

## 8. Characterising a set of applications

For a procurement or a benchmark suite, the question is often not "why is
this slow" but "what kind of code is each of these": which ones a faster
memory system helps, which ones a better network helps, which ones need
more cores. Profile each code once on the same node type, at the same
level, then put the [fingerprints](snapshot.md#fingerprint) side by side.

```bash
# once per node type, so the DRAM and FLOP axes have a reference
miniprof-calibrate -o $MINIPROF_HOME/share/miniprof/calib

# one run per application, same level for all
mpirun -np 192 miniprof -l full -o wrf   ./wrf.exe
mpirun -np 192 miniprof -l full -o lammps lmp -in in.rhodo.scaled
mpirun -np 192 miniprof -l full -o mom6   ./MOM6

miniprof-fingerprint wrf lammps mom6 --html suite.html --csv suite.csv
```

The text output has one column per run; the HTML page draws every
percentage as a bar on the same 0-100 scale, and starts with each run's
class line. Reading it:

- **The class line** is the short answer: the largest parts of the run,
  and what limits the compute part.
- **Where the time goes** separates codes that a faster network or better
  decomposition would help (MPI, rank imbalance) from those it would not.
  MPI is described as latency or bandwidth bound from its message sizes:
  latency-bound codes care about network latency and MPI implementation,
  bandwidth-bound codes about link speed.
- **Compute pipeline** (top-down) separates codes waiting for memory
  (backend memory) from codes limited by the core (backend core,
  frontend, bad speculation). Retiring is the useful share.
- **Use of the node** places each code against the machine: DRAM
  bandwidth near 60% or more of calibrated means more memory channels or
  faster memory will pay off; a high FLOP rate means more or wider cores
  will; both low with high backend memory means memory latency (access
  pattern), where neither helps much.
- **Descriptors** give the absolute numbers behind the shares (GB/s and
  GFLOP/s per node, FLOPs per byte, MPI MB/s per rank), useful for sizing.

`--csv` gives one row per measure and one column per run, ready for a
spreadsheet; `--json` keeps everything, including how each class line was
built. Repeat on a second node type and compare the two tables, or run
`miniprof-diff` per application to see which codes change character on
the new machine.
