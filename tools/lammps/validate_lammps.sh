#!/usr/bin/env bash
# validate_lammps.sh — miniprof snapshot mode on LAMMPS (rhodopsin benchmark,
# MPI + OpenMP), checked against LAMMPS's own timing breakdown and an
# unprofiled run. Run it inside a 1-4 node allocation:
#
#   MINIPROF=$HOME/miniprof LMP=$HOME/lammps-build/build/lmp \
#   MPIRUN_ARGS="-np 96 --map-by ppr:24:node:pe=8 --bind-to none" \
#   BIND=./numa_bind.sh OMP_NUM_THREADS=8 \
#   $MINIPROF/tools/lammps/validate_lammps.sh [runs]
#
#   runs        default "off basic off topdown off roofline off"
#   MINIPROF    the miniprof checkout (libminiprof.so, miniprof-nodemon, tools/)
#   LMP         the lmp binary (tools/lammps/build_lammps.sh)
#   BENCH       LAMMPS bench directory with data.rhodo (default: next to LMP's source)
#   MPIRUN_ARGS your mpirun options; with Open MPI the script adds -x for
#               MINIPROF_* and OMP_*
#   BIND        optional binding wrapper between mpirun and lmp; it must exec "$@"
#   REPLICATE   copies of the 32,000-atom system in x y z (default "4 4 4" = 2.0 M atoms)
#   STEPS       MD steps (default 2000); DUMP_EVERY (default 500) trajectory dumps
#   CALIB       calibration dir for the roofline (default $MINIPROF/calib)
#   KEEP_OUT=1  keep the dump files (default: checksums and sizes only)
# Results go to miniprof_lammps_<date>/; tools/lammps/check_lammps.py checks them.
set -u
MP=${MINIPROF:?set MINIPROF to the miniprof checkout}
LMP=${LMP:?set LMP to the lmp binary}
RUNS=${1:-"off basic off topdown off roofline off"}
read -r RX RY RZ <<< "${REPLICATE:-4 4 4}"
STEPS=${STEPS:-2000}; DUMP_EVERY=${DUMP_EVERY:-500}
CALIB=${CALIB:-$MP/calib}
OMP=${OMP_NUM_THREADS:-1}; export OMP_NUM_THREADS=$OMP
OUT=$PWD/miniprof_lammps_$(date +%Y%m%d_%H%M%S)
[ -x "$LMP" ] || { echo "no lmp at $LMP"; exit 1; }
[ -f "$MP/libminiprof.so" ] || { echo "no $MP/libminiprof.so: make lib"; exit 1; }
[ -n "${MPIRUN_ARGS:-}" ] || { echo "set MPIRUN_ARGS (rank count and mapping)"; exit 1; }
if [ -z "${BENCH:-}" ]; then
    for b in "$(dirname "$LMP")/../lammps/bench" "$(dirname "$LMP")/../bench" "$(dirname "$LMP")/../../bench"; do
        [ -f "$b/data.rhodo" ] && BENCH=$(cd "$b" && pwd) && break
    done
fi
[ -f "${BENCH:-}/data.rhodo" ] || { echo "set BENCH to LAMMPS's bench directory (data.rhodo)"; exit 1; }
mkdir -p "$OUT"
stat -L -c '%s %n' "$BENCH/data.rhodo" > "$OUT/input_size.txt"

# the benchmark input (bench/in.rhodo.scaled) plus trajectory dumps and a
# chosen length; thermo every DUMP_EVERY steps is what the runs are compared on
cat > "$OUT/in.rhodo.miniprof" <<EOF
# bench/in.rhodo.scaled + dumps, for the miniprof validation
variable        x index 1
variable        y index 1
variable        z index 1
units           real
neigh_modify    delay 5 every 1
atom_style      full
atom_modify     map hash
bond_style      harmonic
angle_style     charmm
dihedral_style  charmm
improper_style  harmonic
pair_style      lj/charmm/coul/long 8.0 10.0
pair_modify     mix arithmetic
kspace_style    pppm 1e-4
read_data       $BENCH/data.rhodo
replicate       \$x \$y \$z
fix             1 all shake 0.0001 5 0 m 1.0 a 232
fix             2 all npt temp 300.0 300.0 100.0 &
                z 0.0 0.0 1000.0 mtk no pchain 0 tchain 1
special_bonds   charmm
thermo          $DUMP_EVERY
thermo_style    multi
thermo_modify   norm no
timestep        2.0
dump            1 all custom $DUMP_EVERY dump.rhodo.*.txt id type x y z
dump_modify     1 sort id
run             $STEPS
EOF

{
echo "== environment"; date -u; hostname; uname -r
grep -m1 "model name" /proc/cpuinfo
echo "paranoid $(cat /proc/sys/kernel/perf_event_paranoid)"
echo "MPIRUN_ARGS=$MPIRUN_ARGS"; echo "BIND=${BIND:-}"; echo "OMP_NUM_THREADS=$OMP"
echo "REPLICATE=$RX $RY $RZ ($((32000 * RX * RY * RZ)) atoms) STEPS=$STEPS DUMP_EVERY=$DUMP_EVERY"
mpirun --version 2>&1 | head -1
echo "SLURM_JOB_NODELIST=${SLURM_JOB_NODELIST:-}"
"$LMP" -h 2>/dev/null | grep -m1 "Large-scale Atomic"
"$LMP" -h 2>/dev/null | sed -n '/^Compiler:/p;/^OpenMP standard/p;/^MPI v/p'
(cd "$MP" && git log --oneline -1)
"$MP/tools/nodemon_ready.sh" "$MP/miniprof-nodemon" || echo "(memory helper not ready on this node)"
ls "$CALIB"/calibration_*.json 2>/dev/null || echo "(no calibration in $CALIB: roofline unavailable)"
} 2>&1 | tee "$OUT/environment.txt"

OMPI=0; mpirun --version 2>&1 | grep -qi "open mpi" && OMPI=1
fwd() { [ $OMPI = 1 ] || return 0; for v in "$@"; do [ -n "${!v+x}" ] && printf -- '-x %s ' "$v"; done; }

i=0
for run in $RUNS; do
    i=$((i + 1))
    d="$OUT/$(printf '%02d' $i)_$run"; mkdir -p "$d"
    unset MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT MINIPROF_CALIBRATION MINIPROF_NODEMON
    if [ "$run" = off ]; then
        PRE=()
    else
        export MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=$run MINIPROF_SNAPSHOT_OUTPUT="$d/snapshot.json"
        export MINIPROF_CALIBRATION="$CALIB" MINIPROF_NODEMON="$MP/miniprof-nodemon"
        PRE=(env "LD_PRELOAD=$MP/libminiprof.so")   # lmp only, not mpirun or the wrapper
    fi
    echo; echo "==================== run $i: $run ===================="
    t0=$(date +%s.%N)
    # shellcheck disable=SC2046,SC2086
    (cd "$d" && mpirun $MPIRUN_ARGS $(fwd MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT \
            MINIPROF_CALIBRATION MINIPROF_NODEMON OMP_NUM_THREADS OMP_PLACES OMP_PROC_BIND OMP_STACKSIZE) \
        ${BIND:-} "${PRE[@]}" "$LMP" -in "$OUT/in.rhodo.miniprof" -sf omp -pk omp "$OMP" \
        -var x "$RX" -var y "$RY" -var z "$RZ" -log log.lammps -screen none > stdout.txt 2>&1)
    rc=$?
    t1=$(date +%s.%N)
    echo "$t0 $t1" | awk '{printf "%.3f\n", $2 - $1}' > "$d/wall_s.txt"
    echo "exit $rc, wall $(cat "$d/wall_s.txt") s"
    grep -E "^Loop time|^Performance|MPI tasks x" "$d/log.lammps" 2>/dev/null
    for f in "$d"/dump.rhodo.*.txt; do
        [ -f "$f" ] || continue
        (cd "$d" && md5sum "$(basename "$f")") >> "$d/out_md5.txt"
        stat -c '%s %n' "$f" >> "$d/out_size.txt"
        [ "${KEEP_OUT:-0}" = 1 ] || rm -f "$f"
    done
    if [ -f "$d/snapshot.json" ]; then
        python3 "$MP/tools/miniprof_report.py" "$d/snapshot.json" -o "$d/report.txt" \
            --html "$d/report.html" --csv "$d/csv" 2>&1 | tail -1
    fi
done
unset MINIPROF_MODE MINIPROF_SNAPSHOT MINIPROF_SNAPSHOT_OUTPUT MINIPROF_CALIBRATION MINIPROF_NODEMON

echo; echo "==================== checks ===================="
python3 "$MP/tools/lammps/check_lammps.py" "$OUT" | tee "$OUT/checks.txt"
echo
echo "Results in $OUT/ — please send checks.txt, environment.txt and the basic run's report.html."
