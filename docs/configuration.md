# Configuration

miniprof is configured through environment variables, read when the
application calls `MPI_Init`. The `miniprof` launcher sets the ones it
needs itself; the rest you set in the job script. Without the launcher,
make sure they reach every rank (Open MPI: `mpirun -x VAR`, or
`mpirun ... env VAR=... ./app`).

## Launcher and module

| Variable | Default | Meaning |
|---|---|---|
| `MINIPROF_HOME` | set by the module | installation prefix; the launcher finds the library there |
| `MINIPROF_LEVEL` | `basic` | default `--level` for the launcher |
| `MINIPROF_THRESHOLDS` | unset | thresholds file for the reports ([comparing runs](comparing-runs.md#thresholds)) |

## Mode and output

| Variable | Default | Meaning |
|---|---|---|
| `MINIPROF_MODE` | `sample` | `snapshot`, `sample`, `trace`, or `off` (library loaded but inactive). The launcher sets `snapshot`, or `sample` with `--sample` |
| `MINIPROF_SNAPSHOT_OUTPUT` | `miniprof_snapshot.json` | snapshot path (launcher: `<folder>/snapshot.json`) |
| `MINIPROF_OUTPUT` | `miniprof_report.txt` | sampling report path |
| `MINIPROF_MPA_OUTPUT` | `miniprof.mpa` | functions suggested for tracing |
| `MINIPROF_TRACE_FUNCS` | `miniprof.mpa` | list of functions to trace |
| `MINIPROF_TRACE_OUTPUT` | `miniprof_trace_report.txt` | trace report path |
| `MINIPROF_VERBOSE` | `0` | `1` prints per-rank diagnostics on stderr |

## Snapshot mode

| Variable | Default | Meaning |
|---|---|---|
| `MINIPROF_SNAPSHOT` | `basic` | counter group: `basic`, `topdown`, `roofline`, `full` (the launcher's `--level`) |
| `MINIPROF_HOTSPOTS` | `0` | `1` adds function hotspots (the launcher's `--hotspots`) |
| `MINIPROF_HOTSPOTS_INTERVAL_MS` | `10` | hotspot sampling interval, CPU milliseconds per thread (1–1000) |
| `MINIPROF_TIMELINE` | `0` | `1` adds the [timeline](snapshot.md#timeline) (the launcher's `--timeline`) |
| `MINIPROF_TIMELINE_INTERVAL_MS` | `100` | timeline bucket width to start with (1–60000); doubles as the run grows past 512 buckets |
| `MINIPROF_HOTSPOTS_INLINE` | `1` | `0` names hotspots from the symbol table only (no `addr2line`; inlined functions under their caller) |
| `MINIPROF_COUNTERS_SPLIT` | `1` | `0` stops reading counters at MPI calls: no compute/MPI split, no per-call cost |
| `MINIPROF_NO_RDPMC` | unset | set to read counters with `read()` instead of `rdpmc` |
| `MINIPROF_OMPT` | `1` | `0` leaves OpenMP alone (no OMPT tool) |
| `MINIPROF_OMPT_DEBUG` | unset | log every OMPT callback on stderr (very verbose) |
| `MINIPROF_MEMORY` | `1` | `0` skips the memory and energy helper |
| `MINIPROF_MEMORY_INTERVAL_MS` | `100` | sampling interval for peak bandwidth (10–10000) |
| `MINIPROF_NODEMON` | next to the library | path of `miniprof-nodemon` |
| `MINIPROF_CALIBRATION` | unset | `miniprof-calibrate` output: a file, or a directory (the file for the node's hostname, else one for the same CPU) |
| `MINIPROF_ROOFLINE_PEAK` | widest FP64 | compute roof: `fp64_512`, `fp32_256`, ... |

## Sample and trace modes

| Variable | Default | Meaning |
|---|---|---|
| `MINIPROF_INTERVAL_MS` | `10` | `SIGPROF` sampling interval |
| `MINIPROF_DEPTH` | `8` | call-stack frames kept per sample |
| `MINIPROF_MPA_THRESHOLD` | `1.0` | minimum % of samples for a function to be suggested for tracing |
| `MINIPROF_IPC_COMPUTE_THRESHOLD` | `1.2` | IPC at or above this: COMPUTE-BOUND |
| `MINIPROF_IPC_MEMORY_THRESHOLD` | `0.5` | IPC at or below this, with high MPKI: MEMORY-BOUND |
| `MINIPROF_MPKI_MEMORY_THRESHOLD` | `10` | LLC misses per 1000 instructions for MEMORY-BOUND |

## Symbol resolution

| Variable | Default | Meaning |
|---|---|---|
| `MINIPROF_ADDR2LINE` | `addr2line` | the `addr2line` to use. When GNU `addr2line` returns no line, miniprof retries with `llvm-addr2line`, `llvm-symbolizer` or `eu-addr2line`; setting this variable disables the retry |
| `MINIPROF_ADDR2LINE_ALWAYS` | `0` | `1` runs `addr2line` even for binaries without line information (names come from the symbol table in-process otherwise) |

## Testing only

| Variable | Meaning |
|---|---|
| `MINIPROF_COUNTERS_TEST` | software events instead of hardware counters (task-clock as cycles), so the counter machinery can be tested anywhere |
| `MINIPROF_NODEMON_TEST` | the memory helper reports synthetic values (`1`), or partial coverage (`partial`) |

## Not miniprof's, but relevant

| Variable | Why |
|---|---|
| `OMP_NUM_THREADS`, `OMP_PLACES`, `OMP_PROC_BIND` | thread count and binding: the report checks both |
| `OMP_WAIT_POLICY`, `KMP_BLOCKTIME` | how long OpenMP workers spin between regions (the "OpenMP workers idle" figure) |
| `OMP_TOOL_LIBRARIES` | miniprof adds itself here; another OMPT tool listed first takes precedence |
| `perf_event_paranoid` (sysctl) | ≤ 2 for hardware counters; ≤ 0 lets the memory helper work without a capability |
