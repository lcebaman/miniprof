# Installing miniprof

miniprof is built from source with `make`. An installation is a prefix
with the launcher, the library, the tools and a module file. Users load
the module and put `miniprof` in front of their executable.

- [Build and install](#build-and-install)
- [The module](#the-module)
- [The memory helper (one-off, as root)](#the-memory-helper)
- [Calibration (once per node type)](#calibration)
- [One installation per MPI stack](#one-installation-per-mpi-stack)
- [Checking an installation](#checking-an-installation)
- [Developer setup: the build tree](#developer-setup-the-build-tree)

## Build and install

Load the compiler and MPI you want miniprof to work with, then:

```bash
make install PREFIX=/opt/miniprof/1.1-ompi5 MODULE_VERSION=1.1-ompi5
```

This builds and installs:

| Path | Contents |
|---|---|
| `bin/miniprof` | the launcher (see the [user guide](user-guide.md)) |
| `bin/miniprof-report`, `miniprof-diff`, `miniprof-csv` | report, comparison and CSV tools |
| `bin/miniprof-calibrate` | measures the node's ceilings for the roofline |
| `lib/libminiprof.so` | the profiler library |
| `lib/miniprof-nodemon` | the node-level memory and energy helper |
| `share/miniprof/tools/` | the Python tools behind the `bin/` wrappers |
| `share/miniprof/calib/` | where calibrations go (empty until you run one) |
| `share/modulefiles/miniprof/<MODULE_VERSION>.lua` | Lmod module |
| `share/modulefiles-tcl/miniprof/<MODULE_VERSION>` | Tcl Environment Modules module |
| `include/miniprof.h` | the region API, for code that uses [regions](user-guide.md#regions-timing-parts-of-your-code) |
| `share/doc/miniprof/` | this documentation |

`MODULE_VERSION` defaults to the library version (`1.1.0-dev`). Reinstalling
over the same prefix is safe: jobs already running keep the files they
loaded, new jobs pick up the new ones. Reinstalling replaces
`miniprof-nodemon`, so grant it its capability again (below).

## The module

```bash
module use /opt/miniprof/1.1-ompi5/share/modulefiles      # or put the .lua in your site tree
module load miniprof/1.1-ompi5
module help miniprof                                       # usage, and which MPI it was built for
```

Lmod reads `share/modulefiles`; Tcl Environment Modules read
`share/modulefiles-tcl`. They are kept apart so Lmod does not list the
version twice. `module use` lasts for the current shell only: add it to
`~/.bashrc`, to the job script, or copy the `.lua` file into a modulefiles
tree already on `MODULEPATH`.

The module sets:

| Variable | Value | Purpose |
|---|---|---|
| `PATH` | `<prefix>/bin` | the launcher and tools |
| `MINIPROF_HOME` | `<prefix>` | where the launcher finds the library |
| `MINIPROF_LEVEL` | `basic` | default snapshot level |
| `MINIPROF_CALIBRATION` | `<prefix>/share/miniprof/calib` | roofline ceilings |
| `MINIPROF_NODEMON` | `<prefix>/lib/miniprof-nodemon` | memory helper |

The last three are site defaults: edit them in the module file, or
override them after loading.

The module deliberately does **not** set `LD_PRELOAD`. A module's variables
apply to the whole shell, so a preloaded profiler would be loaded into
every command afterwards (the compiler, `ls`, `mpirun`, Python...). The
launcher sets it for the application only.

## The memory helper

The memory section (DRAM bandwidth per socket and channel, remote-socket
reads, package power and energy) needs node-wide counters, which the
kernel keeps for privileged users unless `perf_event_paranoid <= 0`. Only
the small helper `miniprof-nodemon` gets the capability, never the
application:

```bash
sudo setcap cap_perfmon+ep /opt/miniprof/1.1-ompi5/lib/miniprof-nodemon
```

- Not needed where `perf_event_paranoid` is 0 or lower.
- The filesystem must keep file capabilities. GPFS does; a `nosuid` mount
  does not. Otherwise copy the helper to node-local disk, grant it there,
  and point `MINIPROF_NODEMON` at that copy.
- One rank per node starts it at `MPI_Init`. It opens a fixed set of
  events, drops every capability, samples, and returns run totals and
  peaks over ≥ 10 ms intervals at `MPI_Finalize`.
- In the build tree, `make nodemon-setcap` does the same and also makes the
  helper owner-only (`chmod 0700`), because with the capability it can read
  node-wide energy (RAPL has been root-only since the PLATYPUS side
  channel). Widen access to a group deliberately (`chgrp` + `chmod 0750`).
- `share/miniprof/tools/nodemon_ready.sh <helper>` tells you whether it
  will work on this node.

Memory counters are implemented for AMD Zen 4, 5 and 6; see
[hardware support](hardware.md#memory). Without the helper, the snapshot
says why and everything else is unaffected.

## Calibration

The roofline compares a run against what the node can actually do. Measure
it once per node type, on an exclusive node:

```bash
srun -N1 --exclusive miniprof-calibrate -o /opt/miniprof/1.1-ompi5/share/miniprof/calib
```

It takes about a minute, pins one thread to every physical core, and
writes `calibration_<hostname>.json`: DRAM bandwidth per socket and node
(read-only and triad) and peak FLOP rates for every vector width and
precision. A real one, from a two-socket EPYC 9654 node:

```
miniprof-calibrate: AMD EPYC 9654 96-Core Processor, 2 sockets, 192 physical cores used, arrays 24.00 GiB each
  socket 0 (96 cores): read 387.7 GB/s, triad 383.4 GB/s (DRAM view)
  socket 1 (96 cores): read 388.0 GB/s, triad 382.9 GB/s (DRAM view)
  node: read 767.7 GB/s, triad 763.6 GB/s
  fp64_scalar  all cores     2781.9 GFLOP/s   one core    14.7
  fp64_256     all cores    11107.9 GFLOP/s   one core    58.9
  fp64_512     all cores    11148.6 GFLOP/s   one core    58.9
  ...
```

When `MINIPROF_CALIBRATION` is a directory, a run uses the file for its
node's hostname, or else any file for the same CPU model, so one
calibration covers every node of a type. Calibrations from different node
types can sit side by side.

## One installation per MPI stack

`libminiprof.so` wraps MPI through the PMPI interface and is bound to the
MPI library it was built against (its soname, e.g. Open MPI's
`libmpi.so.40`). An application built with another MPI cannot be
profiled by it: the launcher checks this and runs the application
unprofiled, with a message. Install once per MPI your users run with and
name the modules after it:

```bash
module load aocc openmpi/5.0
make install PREFIX=/opt/miniprof/1.1-ompi5 MODULE_VERSION=1.1-ompi5
make clean
module swap openmpi intel-mpi
make install PREFIX=/opt/miniprof/1.1-impi MODULE_VERSION=1.1-impi
```

The compiler used to build miniprof does not have to match the
application's.

## Checking an installation

On a compute node, with the module loaded:

```
$ miniprof --check $(which lmp)
miniprof 1.1.0-dev in /opt/miniprof/1.1-ompi5
application: /home/user/lammps/build/lmp
MPI: /opt/openmpi/5.0.10/aocc/6.0.0/lib/libmpi.so.40
OpenMP: libomp.so
perf_event_paranoid: 2
memory helper: ready: /opt/miniprof/1.1-ompi5/lib/miniprof-nodemon has CAP_PERFMON
calibration: /opt/miniprof/1.1-ompi5/share/miniprof/calib (3 file(s))
```

| Line | Means |
|---|---|
| `cannot profile: ...` | statically linked, or a different MPI: the run will be unprofiled |
| `OpenMP: libgomp...` | GCC's runtime: no OpenMP section (no OMPT), everything else works |
| `perf_event_paranoid: 3` or more | no hardware counters for users; ask for 2 |
| `memory helper: not ready` | grant the capability, or no memory section |
| `calibration: none` | no roofline until you calibrate |

## Developer setup: the build tree

To work on miniprof itself, everything runs from the checkout:

```bash
make lib nodemon calibrate     # libminiprof.so, miniprof-nodemon, miniprof-calibrate
make check                     # the test suite (see development.md)
LD_PRELOAD=$PWD/libminiprof.so MINIPROF_MODE=snapshot mpirun -n 4 ./examples/heat2d
python3 tools/miniprof_report.py miniprof_snapshot.json --html report.html
```

`modulefiles/miniprof/1.0.lua` is an older module for the build tree that
sets `LD_PRELOAD` for the whole shell, so every MPI program you then run is
sampled. It is convenient for quick sampling experiments but has the
drawback described above; prefer `make install` and the launcher.
