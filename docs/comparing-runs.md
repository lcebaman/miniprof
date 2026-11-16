# Comparing runs, CSV and thresholds

- [miniprof-diff](#miniprof-diff)
- [CSV export](#csv-export)
- [Thresholds](#thresholds)

## miniprof-diff

`miniprof-diff` explains why run B took more or less time than run A: two
CPU generations, two compilers, two layouts, two node pairs, or before and
after a code change.

```bash
miniprof-diff A/snapshot.json B/snapshot.json --label-a Genoa --label-b Turin
miniprof-diff A/snapshot.json B/snapshot.json --html diff.html -o diff.txt
```

It starts with a verdict, then the evidence. From the test fixtures
(synthetic data shipped in `tests/fixtures/`):

```
 * B is 1.29x slower than A (42.00 s -> 54.15 s, slowest rank).
 * Per average rank the run lost 11.46 s; compute accounts for 9.89 s of it (86% of the change).
 * Compute time ratio 1.31: driven mostly by IPC (instructions per cycle) (1.38 -> 1.21, x0.88), which lengthened it (48% of the log change).
 * Largest region change: advance_uv, 9.80 s -> 12.88 s per rank (+3.08 s, x1.31).
 * Largest MPI change: MPI_Waitall, 4.12 s -> 4.73 s per rank (+0.62 s).
 * Top-down (compute): largest shift is backend bound, 52% -> 58% of slots; backend-memory 38% -> 44%.
 * Energy to solution (package, all nodes): 59.5 kJ -> 69.3 kJ (x1.17) at 708 W -> 640 W.

 Compute time explained: T = instructions / (IPC x clock x busy cores), per rank
                                   A              B         B/A   share of change
   instructions per rank           6.539e+11       6.67e+11     1.020            7%
   IPC (instructions per cycle)         1.38           1.21     0.877           48%
   clock (effective GHz)                4.18            3.7     0.885           45%
   busy cores per rank                   3.6            3.6     1.000            0%
   compute time ratio                                           1.314   (product of the factors: 1.314)
```

What it compares:

| Section | Contents |
|---|---|
| where the time went | compute, MPI and I/O per average rank, and each one's share of the change |
| compute time explained | the compute change split exactly into instructions per rank, IPC, clock and busy cores per rank. The ratios multiply back to the time ratio; the shares are of the log of the ratio, so they add up to 100%. Needs the `basic` level in both runs |
| counters | IPC, clock, GFLOP/s, DRAM fills... side by side |
| top-down | the largest shifts in pipeline slots (both runs at `topdown`) |
| regions | time and GFLOP/s per region |
| MPI functions | time per rank and calls, largest changes first |
| hotspots | functions listed in both runs, largest changes in CPU per rank (both with `--hotspots`) |
| memory | bandwidth, power, energy, DRAM moved |
| roofline | both points, each against its own machine's ceilings |
| OpenMP | serial share, imbalance, lock wait |

When compute time changed by 2% or less, it says "compute time unchanged"
and lists any factors that moved but cancel out (more instructions at
higher IPC, for example), instead of shares of a near-zero change.

Things to keep in mind:

- Compare like with like: same input, same rank and thread counts, same
  level. The header shows each run's command, ranks, nodes and level;
  check them before reading on.
- Each run is measured against its own machine's ceilings, so the
  roofline shows which run uses its hardware better, not only which is
  faster.
- A single pair of runs includes run-to-run noise (typically 1-3%).
  Differences of that size are not findings.

[Examples](examples.md#4-why-is-one-node-pair-slower) shows a real
comparison of two node pairs.

## CSV export

```bash
miniprof-report snapshot.json --csv csv/      # or: miniprof-csv snapshot.json csv/
```

One file per table, one row per entity, nested values as dotted column
names (`time_s.avg`):

| File | Rows |
|---|---|
| `summary.csv` | the run (one row) |
| `ranks.csv` | ranks |
| `mpi_functions.csv`, `message_sizes.csv` | MPI functions; size bins per class |
| `io.csv` | I/O by interface |
| `regions.csv` | user regions, with counters, top-down and roofline joined by name |
| `openmp_regions.csv` | OpenMP regions |
| `memory_nodes.csv`, `memory_sockets.csv`, `memory_channels.csv` | memory |
| `roofline_nodes.csv` | roofline per node |
| `hotspots.csv` | hotspot functions |

Files for blocks a run did not collect are not written. The launcher
writes them into the result folder's `csv/`.

## Thresholds

The observations at the end of a report are triggered by thresholds:

| Key | Default | Flags |
|---|---|---|
| `mpi_pct` | 20 | MPI above this % of the run |
| `imbalance_pct` | 10 | compute imbalance across ranks above this % |
| `io_pct` | 10 | I/O above this % of the run |
| `cpu_util_pct` | 80 | CPU utilisation per core below this % |
| `remote_pct` | 20 | remote-socket share of DRAM reads above this % |
| `packed_pct` | 50 | packed (vector) FP operations below this % |
| `serial_pct` | 20 | OpenMP serial time above this % of the run |
| `omp_imbalance_pct` | 10 | OpenMP barrier or lock wait above this % of thread time |
| `omp_idle_pct` | 20 | OpenMP worker spin between regions above this % of cycles |
| `td_memory_pct` | 30 | top-down backend memory (or core) above this % of slots |
| `td_frontend_pct` | 20 | top-down frontend bound above this % |
| `td_badspec_pct` | 10 | top-down bad speculation above this % |
| `roof_dram_pct` | 50 | a memory-bound run below this % of the DRAM ceiling |
| `roof_peak_pct` | 20 | a compute-bound run below this % of peak FLOPs |

To change them, write the defaults to a file, edit it, and use it:

```bash
miniprof-report --write-thresholds my_thresholds.ini
miniprof-report snapshot.json --thresholds my_thresholds.ini
export MINIPROF_THRESHOLDS=$PWD/my_thresholds.ini     # also picked up by the launcher
miniprof-report snapshot.json --mpi-pct 30            # one-off; overrides the file
```

The file is INI format with a `[thresholds]` section. One per benchmark is
a good habit: a code that is meant to spend 30% in MPI should not be
flagged for it every time.
