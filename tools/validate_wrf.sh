#!/usr/bin/env bash
# validate_wrf.sh — miniprof snapshot mode on a real WRF run, checked
# against WRF's own timings (rsl.error.0000) and an unprofiled run.
#
# Run it from a prepared WRF run directory (namelist.input, wrfinput,
# wrfbdy, ... in place), inside a 2-4 node allocation:
#
#   MINIPROF=/path/to/miniprof \
#   MPIRUN_ARGS="-np 64 --map-by ppr:16:node:pe=6 --bind-to none" \
#   BIND="./bind_numactl.sh" OMP_NUM_THREADS=6 \
#   $MINIPROF/tools/validate_wrf.sh [runs]
#
#   runs      which runs, default "off basic off topdown off roofline off"
#   MINIPROF  the miniprof checkout (libminiprof.so, miniprof-nodemon, tools/)
#   MPIRUN_ARGS  your mpirun options (rank count, mapping); with Open MPI the
#             script adds -x for MINIPROF_* and OMP_*
#   BIND      optional wrapper your launch uses (numactl script), put
#             between mpirun and wrf.exe; it must exec "$@"; empty for none
#   WRF_EXE   default ./wrf.exe
#   CALIB     calibration dir for the roofline (default $MINIPROF/calib)
#   KEEP_OUT  1 = keep each run's wrfout/history files (default: checksums only)
#
# Each run's rsl.error.0000, snapshot, wall time and output checksums go to
# miniprof_wrf_<date>/<run>/; then tools/check_wrf.py compares everything.
set -u
MP=${MINIPROF:?set MINIPROF to the miniprof checkout}
RUNS=${1:-"off basic off topdown off roofline off"}
EXE=${WRF_EXE:-./wrf.exe}
CALIB=${CALIB:-$MP/calib}
OUT=miniprof_wrf_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
[ -x "$EXE" ] || { echo "no $EXE here (cd to the WRF run directory or set WRF_EXE)"; exit 1; }
[ -f "$MP/libminiprof.so" ] || { echo "no $MP/libminiprof.so: make lib"; exit 1; }
[ -n "${MPIRUN_ARGS:-}" ] || { echo "set MPIRUN_ARGS (rank count and mapping you normally use)"; exit 1; }

{
echo "== environment"; date -u; hostname; uname -r
grep -m1 "model name" /proc/cpuinfo
echo "paranoid $(cat /proc/sys/kernel/perf_event_paranoid)"
echo "MPIRUN_ARGS=$MPIRUN_ARGS"; echo "BIND=${BIND:-}"; echo "OMP_NUM_THREADS=${OMP_NUM_THREADS:-unset}"
mpirun --version 2>&1 | head -1
echo "SLURM_JOB_NODELIST=${SLURM_JOB_NODELIST:-}"
grep -E "^\s*(run_|time_step|e_we|e_sn|e_vert|history_interval|io_form_history|io_form_restart|nproc_|numtiles)" namelist.input
(cd "$MP" && git log --oneline -1)
"$MP/tools/nodemon_ready.sh" "$MP/miniprof-nodemon" || echo "(memory helper not ready: the memory block will be unavailable)"
ls "$CALIB"/calibration_*.json 2>/dev/null || echo "(no calibration in $CALIB: roofline unavailable; run $MP/miniprof-calibrate -o $CALIB on one node)"
} 2>&1 | tee "$OUT/environment.txt"

for f in wrfinput_d0* wrfbdy_d0*; do [ -f "$f" ] && stat -L -c '%s %n' "$f"; done > "$OUT/input_size.txt"

# Open MPI forwards only the variables named with -x; MPICH and Intel MPI
# (hydra) forward the whole environment.
OMPI=0; mpirun --version 2>&1 | grep -qi "open mpi" && OMPI=1
fwd() {  # -x for every variable that is set
    [ $OMPI = 1 ] || return 0
    for v in "$@"; do [ -n "${!v+x}" ] && printf -- '-x %s ' "$v"; done
}

i=0
for run in $RUNS; do
    i=$((i + 1))
    d="$OUT/$(printf '%02d' $i)_$run"; mkdir -p "$d"
    rm -f rsl.error.* rsl.out.* wrfout_d0* wrfrst_d0* 2>/dev/null
    unset MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT MINIPROF_CALIBRATION MINIPROF_NODEMON
    if [ "$run" = off ]; then
        PRE=()                  # unprofiled: miniprof not loaded at all
    else
        export MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=$run
        export MINIPROF_SNAPSHOT_OUTPUT="$PWD/$d/snapshot.json" MINIPROF_CALIBRATION="$CALIB"
        export MINIPROF_NODEMON="$MP/miniprof-nodemon"
        # preloaded into wrf.exe only, after the binding wrapper, so neither
        # mpirun nor numactl carries the library
        PRE=(env "LD_PRELOAD=$MP/libminiprof.so")
    fi
    echo; echo "==================== run $i: $run ===================="
    t0=$(date +%s.%N)
    # shellcheck disable=SC2046,SC2086
    mpirun $MPIRUN_ARGS $(fwd MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT \
            MINIPROF_CALIBRATION MINIPROF_NODEMON OMP_NUM_THREADS OMP_PLACES \
            OMP_PROC_BIND OMP_STACKSIZE) \
        ${BIND:-} "${PRE[@]}" "$EXE" > "$d/stdout.txt" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    echo "$t0 $t1" | awk '{printf "%.3f\n", $2 - $1}' > "$d/wall_s.txt"
    echo "exit $rc, wall $(cat "$d/wall_s.txt") s"
    cp rsl.error.0000 "$d/" 2>/dev/null
    tail -2 rsl.error.0000 2>/dev/null
    grep -q "SUCCESS COMPLETE WRF" rsl.error.0000 2>/dev/null || echo "!! WRF did not report SUCCESS COMPLETE"
    for f in wrfout_d0* wrfrst_d0*; do
        [ -f "$f" ] || continue
        # WRF stamps no creation time into its files, so identical runs give
        # identical bytes; check_wrf.py first checks the two unprofiled runs agree
        md5sum "$f" >> "$d/wrfout_md5.txt"
        stat -c '%s %n' "$f" >> "$d/wrfout_size.txt"
        [ "${KEEP_OUT:-0}" = 1 ] && cp "$f" "$d/"
    done
    if [ -f "$d/snapshot.json" ]; then
        python3 "$MP/tools/miniprof_report.py" "$d/snapshot.json" -o "$d/report.txt" \
            --html "$d/report.html" --csv "$d/csv" 2>&1 | tail -1
    fi
done
unset MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT MINIPROF_CALIBRATION MINIPROF_NODEMON

echo; echo "==================== checks ===================="
python3 "$MP/tools/check_wrf.py" "$OUT" | tee "$OUT/checks.txt"
echo
echo "Results in $OUT/ — please send checks.txt, environment.txt and the basic run's report.html."
