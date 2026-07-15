-- miniprof — transparent MPI sampling/tracing profiler
--
-- Loading this module preloads libminiprof.so over every MPI rank via
-- LD_PRELOAD. Sample mode then requires zero changes to the user's
-- source or build: a completely normal `mpicc`/`mpifort` build, run
-- with a completely normal `mpirun`, produces a sampling report at
-- MPI_Finalize.
--
-- This file must live at <miniprof-repo>/modulefiles/miniprof/<version>.lua
-- so it can locate libminiprof.so relative to itself; it assumes the
-- library has already been built there via `make lib`.

help([[
miniprof — lightweight MPI sampling/tracing profiler (LD_PRELOAD-based)

SAMPLE MODE (transparent — no rebuild needed):

  module load miniprof
  mpirun -n 64 ./your_app

A sampling report and a .mpa file (suggested hot functions worth
tracing) are written when the job calls MPI_Finalize.

-g in your own build is recommended for source-line attribution, but not
required: function-level profiling still works without it, via an
nm-based symbol-table fallback.

TRACE MODE (exact per-function timing) is NOT transparent — instrumentation
calls are inserted by the compiler, so the target binary must be rebuilt
with -finstrument-functions and linked against libminiprof.so. Use the
.mpa file from a sampling run to target only the functions worth tracing:

  mpicc -O2 -g -finstrument-functions your_app.c \
      -L$MINIPROF_ROOT -lminiprof \
      -Wl,-rpath,$MINIPROF_ROOT -Wl,--allow-shlib-undefined \
      -ldl -lm -o your_app+trace
  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=miniprof.mpa \
      mpirun -n 64 ./your_app+trace

Key environment variables (sample mode):
  MINIPROF_INTERVAL_MS    sampling interval in ms      (default: 10)
  MINIPROF_DEPTH          call-stack sample depth      (default: 8)
  MINIPROF_OUTPUT         report path                  (default: miniprof_report.txt)
  MINIPROF_MPA_OUTPUT     .mpa suggestions path         (default: miniprof.mpa)
  MINIPROF_MPA_THRESHOLD  min Samp%% to suggest           (default: 1.0)
  MINIPROF_VERBOSE        1 for per-rank stderr logging

Full documentation: $MINIPROF_ROOT/README.md
]])

whatis("Name: miniprof")
whatis("Version: 1.0")
whatis("Description: Transparent MPI sampling/tracing profiler, enabled via LD_PRELOAD")

-- Locate the repo root from this modulefile's own path rather than a
-- hardcoded install prefix, so the same file works regardless of where
-- the module tree is mounted.
local fileName = myFileName()
local miniprofRoot = fileName:match("(.*)/modulefiles/")

if miniprofRoot == nil then
    LmodError("miniprof: could not determine MINIPROF_ROOT from modulefile path '"
              .. fileName .. "' — expected .../modulefiles/miniprof/<version>.lua")
end

local libPath = pathJoin(miniprofRoot, "libminiprof.so")

if not isFile(libPath) then
    LmodError("miniprof: " .. libPath .. " not found — run `make lib` in "
              .. miniprofRoot .. " first")
end

setenv("MINIPROF_ROOT", miniprofRoot)
prepend_path("LD_PRELOAD", libPath)
