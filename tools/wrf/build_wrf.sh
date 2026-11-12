#!/usr/bin/env bash
# build_wrf.sh — build WRF (dm+sm) with netCDF and PnetCDF (io_form 11) for
# the miniprof validation run, and set up the conus12km case.
#
#   tools/wrf/build_wrf.sh all [CASE_TARBALL]   deps + wrf (+ case)
#   tools/wrf/build_wrf.sh deps                 HDF5, netCDF-C/Fortran, PnetCDF
#   tools/wrf/build_wrf.sh wrf                  WRF em_real, dm+sm
#   tools/wrf/build_wrf.sh case CASE_TARBALL    run directory from the NCAR
#                                               v4.4 conus12km benchmark tar
#
# Environment (all optional):
#   TOOLCHAIN  aocc | intel | gcc (default: the first found, in that order).
#              aocc or intel are best for miniprof: their OpenMP runtimes
#              (libomp, libiomp5) support OMPT, so the OpenMP block is filled;
#              GCC's libgomp has no OMPT.
#   MPI        the MPI wrappers (mpicc/mpif90) on PATH must be built with the
#              same Fortran compiler (checked below)
#   PREFIX     where everything goes (default ./wrf-build)
#   JOBS       parallel make jobs (default nproc)
#   MARCH      target CPU (default znver4 = Genoa; Intel default core-avx2,
#              never -x..., which refuses to run on AMD)
#   WRF_VERSION default v4.8.0
#   NETCDF, PNETCDF  existing installs (netCDF-C and -Fortran in one prefix);
#              skips building them
#   SRC_DIR    pre-fetched sources for air-gapped clusters (git checkouts
#              named hdf5, netcdf-c, netcdf-fortran, WRF, and
#              pnetcdf-<version>.tar.gz); default
#              $PREFIX/src, cloned from GitHub when missing
set -euo pipefail

STEP=${1:-all}
PREFIX=$(mkdir -p "${PREFIX:-$PWD/wrf-build}" && cd "${PREFIX:-$PWD/wrf-build}" && pwd)
SRC_DIR=${SRC_DIR:-$PREFIX/src}
DEPS=$PREFIX/deps
JOBS=${JOBS:-$(nproc)}
WRF_VERSION=${WRF_VERSION:-v4.8.0}
HDF5_TAG=${HDF5_TAG:-hdf5_1.14.6}
NETCDF_C_TAG=${NETCDF_C_TAG:-v4.9.3}
NETCDF_F_TAG=${NETCDF_F_TAG:-v4.6.2}
PNETCDF_VERSION=${PNETCDF_VERSION:-1.14.1}
LOG=$PREFIX/logs; mkdir -p "$SRC_DIR" "$LOG"

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\n!! %s\n' "$*" >&2; exit 1; }

# ── toolchain ──────────────────────────────────────────────────────────────
TOOLCHAIN=${TOOLCHAIN:-auto}
if [ "$TOOLCHAIN" = auto ]; then
    if command -v flang >/dev/null && clang --version 2>/dev/null | grep -qi aocc; then TOOLCHAIN=aocc
    elif command -v ifx >/dev/null; then TOOLCHAIN=intel
    else TOOLCHAIN=gcc; fi
fi
case $TOOLCHAIN in
  aocc)  CC=clang; CXX=clang++; FC=flang; MARCH=${MARCH:-znver4}; WRF_ROW='AMD \(flang/clang\)' ;;
  intel) CC=icx;   CXX=icpx;    FC=ifx;   MARCH=${MARCH:-core-avx2}; WRF_ROW='INTEL \(ifx/icx\)' ;;
  gcc)   CC=gcc;   CXX=g++;     FC=gfortran; MARCH=${MARCH:-znver4}; WRF_ROW='GNU \(gfortran/gcc\)' ;;
  *) die "TOOLCHAIN must be aocc, intel or gcc" ;;
esac
for c in $CC $FC mpicc mpif90 make cmake m4 perl; do
    command -v "$c" >/dev/null || die "$c not found on PATH (load the $TOOLCHAIN, MPI and cmake modules)"
done
command -v csh >/dev/null || command -v tcsh >/dev/null || die "WRF's compile script needs csh/tcsh"
export CC CXX FC F77=$FC F90=$FC
# point the MPI wrappers at this toolchain (Open MPI, MPICH, Intel MPI)
export OMPI_CC=$CC OMPI_CXX=$CXX OMPI_FC=$FC MPICH_CC=$CC MPICH_CXX=$CXX MPICH_FC=$FC \
       I_MPI_CC=$CC I_MPI_CXX=$CXX I_MPI_F90=$FC I_MPI_FC=$FC
MPI_FC_REAL=$( (mpif90 --showme:command 2>/dev/null || mpif90 -show 2>/dev/null) | awk '{print $1}')
MPI_KIND=other; mpirun --version 2>&1 | grep -qi "open mpi" && MPI_KIND=openmpi

# Fortran modules are compiler-specific: if the MPI was built with another
# compiler, `use mpi` fails at WRF's first MPI file. Check before an hour of build.
check_mpi_fortran() {
    local t; t=$(mktemp -d)
    printf 'program t\n use mpi\n integer :: e\n call MPI_Init(e)\n call MPI_Finalize(e)\nend\n' > "$t/t.f90"
    if ! (cd "$t" && mpif90 t.f90 -o t >"$t/log" 2>&1); then
        cat "$t/log"
        die "mpif90 ($MPI_FC_REAL) cannot compile 'use mpi' with $FC: load an MPI built with $TOOLCHAIN"
    fi
    rm -rf "$t"
}

fetch() {  # fetch NAME URL TAG
    local d=$SRC_DIR/$1
    [ -d "$d" ] && return 0
    say "cloning $1 $3"
    git clone -q --depth 1 --recurse-submodules --shallow-submodules -b "$3" "$2" "$d" \
        2>&1 | grep -v "detached HEAD\|^$\|git switch\|advice\|Note:\|you can\|changes and\|state without\|do so\|Or undo\|Turn off\|If you want" || true
    [ -d "$d" ] || die "could not fetch $1: put a checkout of $2 ($3) in $d"
}

# ── dependencies ───────────────────────────────────────────────────────────
build_deps() {
    if [ -n "${NETCDF:-}" ] && [ -n "${PNETCDF:-}" ]; then
        say "using NETCDF=$NETCDF and PNETCDF=$PNETCDF"; return 0
    fi
    mkdir -p "$DEPS"
    local R="-Wl,-rpath,$DEPS/lib"
    export CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib $R"
    export PATH=$DEPS/bin:$PATH

    if [ ! -f "$DEPS/lib/libhdf5.so" ]; then
        fetch hdf5 https://github.com/HDFGroup/hdf5 "$HDF5_TAG"
        say "HDF5 (serial, C only)  [log: $LOG/hdf5.log]"
        cmake -S "$SRC_DIR/hdf5" -B "$PREFIX/obj/hdf5" -DCMAKE_INSTALL_PREFIX="$DEPS" \
              -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DHDF5_BUILD_TOOLS=OFF \
              -DHDF5_BUILD_EXAMPLES=OFF -DHDF5_BUILD_FORTRAN=OFF -DHDF5_BUILD_CPP_LIB=OFF \
              -DHDF5_BUILD_HL_LIB=ON -DHDF5_ENABLE_Z_LIB_SUPPORT=ON -DHDF5_ENABLE_SZIP_SUPPORT=OFF \
              -DCMAKE_INSTALL_RPATH="$DEPS/lib" > "$LOG/hdf5.log" 2>&1
        cmake --build "$PREFIX/obj/hdf5" -j "$JOBS" >> "$LOG/hdf5.log" 2>&1
        cmake --install "$PREFIX/obj/hdf5" >> "$LOG/hdf5.log" 2>&1
    fi
    if [ ! -f "$DEPS/lib/libnetcdf.so" ]; then
        fetch netcdf-c https://github.com/Unidata/netcdf-c "$NETCDF_C_TAG"
        say "netCDF-C  [log: $LOG/netcdf-c.log]"
        cmake -S "$SRC_DIR/netcdf-c" -B "$PREFIX/obj/netcdf-c" -DCMAKE_INSTALL_PREFIX="$DEPS" \
              -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS" -DHDF5_ROOT="$DEPS" \
              -DENABLE_DAP=OFF -DNETCDF_ENABLE_DAP=OFF -DENABLE_BYTERANGE=OFF -DNETCDF_ENABLE_BYTERANGE=OFF \
              -DENABLE_NCZARR=OFF -DNETCDF_ENABLE_NCZARR=OFF -DENABLE_PLUGINS=OFF -DNETCDF_ENABLE_PLUGINS=OFF \
              -DENABLE_TESTS=OFF -DNETCDF_ENABLE_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_LIBXML2=OFF \
              -DNETCDF_ENABLE_LIBXML2=OFF -DENABLE_FILTER_TESTING=OFF -DCMAKE_INSTALL_LIBDIR=lib \
              -DCMAKE_INSTALL_RPATH="$DEPS/lib" > "$LOG/netcdf-c.log" 2>&1
        cmake --build "$PREFIX/obj/netcdf-c" -j "$JOBS" >> "$LOG/netcdf-c.log" 2>&1
        cmake --install "$PREFIX/obj/netcdf-c" >> "$LOG/netcdf-c.log" 2>&1
    fi
    if [ ! -f "$DEPS/lib/libnetcdff.so" ]; then
        fetch netcdf-fortran https://github.com/Unidata/netcdf-fortran "$NETCDF_F_TAG"
        say "netCDF-Fortran  [log: $LOG/netcdf-fortran.log]"
        cmake -S "$SRC_DIR/netcdf-fortran" -B "$PREFIX/obj/netcdf-fortran" -DCMAKE_INSTALL_PREFIX="$DEPS" \
              -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS" -DnetCDF_DIR="$DEPS/lib/cmake/netCDF" \
              -DENABLE_TESTS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DCMAKE_INSTALL_LIBDIR=lib \
              -DCMAKE_INSTALL_RPATH="$DEPS/lib" > "$LOG/netcdf-fortran.log" 2>&1
        cmake --build "$PREFIX/obj/netcdf-fortran" -j "$JOBS" >> "$LOG/netcdf-fortran.log" 2>&1
        cmake --install "$PREFIX/obj/netcdf-fortran" >> "$LOG/netcdf-fortran.log" 2>&1
    fi
    if [ ! -f "$DEPS/lib/libpnetcdf.a" ] && [ ! -f "$DEPS/lib/libpnetcdf.so" ]; then
        # the release tarball ships configure; a git checkout needs the newest libtool
        local tgz=$SRC_DIR/pnetcdf-$PNETCDF_VERSION.tar.gz
        if [ ! -f "$tgz" ]; then
            say "downloading PnetCDF $PNETCDF_VERSION"
            for u in "https://parallel-netcdf.github.io/Release/pnetcdf-$PNETCDF_VERSION.tar.gz" \
                     "https://raw.githubusercontent.com/Parallel-NetCDF/Parallel-NetCDF.github.io/master/Release/pnetcdf-$PNETCDF_VERSION.tar.gz"; do
                curl -fsSL --max-time 300 -o "$tgz.part" "$u" 2>/dev/null && mv "$tgz.part" "$tgz" && break
            done
            [ -f "$tgz" ] || die "could not download PnetCDF: put pnetcdf-$PNETCDF_VERSION.tar.gz in $SRC_DIR"
        fi
        say "PnetCDF  [log: $LOG/pnetcdf.log]"
        ( rm -rf "$SRC_DIR/pnetcdf-$PNETCDF_VERSION" && tar -xzf "$tgz" -C "$SRC_DIR"
          mkdir -p "$PREFIX/obj/pnetcdf" && cd "$PREFIX/obj/pnetcdf"
          "$SRC_DIR/pnetcdf-$PNETCDF_VERSION/configure" --prefix="$DEPS" MPICC=mpicc MPICXX=mpicxx MPIF77=mpif90 \
              MPIF90=mpif90 --disable-cxx --enable-shared --disable-silent-rules > "$LOG/pnetcdf.log" 2>&1
          make -j "$JOBS" >> "$LOG/pnetcdf.log" 2>&1
          make install >> "$LOG/pnetcdf.log" 2>&1 ) || die "PnetCDF failed: see $LOG/pnetcdf.log"
    fi
    export NETCDF=$DEPS PNETCDF=$DEPS
    say "netCDF $("$DEPS/bin/nc-config" --version) (netCDF-4: $("$DEPS/bin/nc-config" --has-nc4)), " \
        "netCDF-Fortran $("$DEPS/bin/nf-config" --version 2>/dev/null), " \
        "PnetCDF $("$DEPS/bin/pnetcdf-config" --version 2>/dev/null | awk '{print $NF}')"
}

# ── WRF ────────────────────────────────────────────────────────────────────
build_wrf() {
    export NETCDF=${NETCDF:-$DEPS} PNETCDF=${PNETCDF:-$DEPS}
    [ -f "$NETCDF/lib/libnetcdff.so" ] || [ -f "$NETCDF/lib/libnetcdff.a" ] || [ -f "$NETCDF/lib64/libnetcdff.so" ] \
        || die "no netCDF-Fortran in NETCDF=$NETCDF (run: $0 deps)"
    check_mpi_fortran
    fetch WRF https://github.com/wrf-model/WRF "$WRF_VERSION"
    local W=$SRC_DIR/WRF
    cd "$W"
    # HDF5 set would add WRF's own HDF5 I/O layer (needs HDF5 Fortran): keep it off
    unset HDF5 PHDF5 ADIOS2 NETCDFPAR
    export WRFIO_NCD_LARGE_FILE_SUPPORT=1
    if [ ! -f configure.wrf ] || [ "${RECONFIGURE:-0}" = 1 ]; then
        say "configure WRF $WRF_VERSION, $TOOLCHAIN dm+sm"
        local opt
        # configure re-prompts forever on EOF: keep the first listing only
        ( set +o pipefail; timeout 120 ./configure < /dev/null 2>&1 | head -400 > "$LOG/wrf-options.log" ) || true
        opt=$(grep -E "\(dm\+sm\).*$WRF_ROW" "$LOG/wrf-options.log" | head -1 |
              sed -E 's/.* ([0-9]+)\. \(dm\+sm\).*/\1/' || true)
        [ -n "$opt" ] && [ "$opt" -eq "$opt" ] 2>/dev/null || die "no dm+sm row for $WRF_ROW in ./configure"
        printf '%s\n1\n' "$opt" | ./configure > "$LOG/wrf-configure.log" 2>&1 \
            || die "WRF configure failed: see $LOG/wrf-configure.log"
        grep -q "PNETCDF" configure.wrf || die "configure.wrf has no PnetCDF: is PNETCDF set?"
        # local adjustments: target CPU, rpath to the libraries, plain wrappers
        local R="-Wl,-rpath,$NETCDF/lib -Wl,-rpath,$PNETCDF/lib"
        case $TOOLCHAIN in
          aocc)  sed -i "s/^AMDARCH .*/AMDARCH         =       -march=$MARCH/" configure.wrf ;;
          intel) sed -i -E "s/^(FCOPTIM\s*=.*)/\1 -march=$MARCH/; s/^(CFLAGS_LOCAL\s*=\s*)/\1-march=$MARCH /" configure.wrf
                 # -f90=/-cc= are MPICH/Intel MPI options; Open MPI uses OMPI_FC/OMPI_CC
                 [ $MPI_KIND = openmpi ] && sed -i -E 's/^(DM_FC\s*=\s*)mpif90 .*/\1mpif90/; s/^(DM_CC\s*=\s*)mpicc .*/\1mpicc/' configure.wrf ;;
          gcc)   sed -i -E "s/^(FCOPTIM\s*=.*)/\1 -march=$MARCH/; s/^(CFLAGS_LOCAL\s*=\s*)/\1-march=$MARCH /" configure.wrf ;;
        esac
        sed -i -E "s|^(LDFLAGS_LOCAL\s*=\s*)|\1$R |" configure.wrf
        grep -E "^(DM_FC|DM_CC|FCOPTIM|CFLAGS_LOCAL|LDFLAGS_LOCAL|OMP |AMDARCH)\s*=" configure.wrf
    fi
    say "compile em_real with $JOBS jobs  [log: $LOG/wrf-compile.log; 30-60 min]"
    ./compile -j "$JOBS" em_real > "$LOG/wrf-compile.log" 2>&1 || true
    [ -x main/wrf.exe ] || { grep -n -m20 -iE " error|Error [0-9]" "$LOG/wrf-compile.log"; die "wrf.exe not built: see $LOG/wrf-compile.log"; }
    say "built $W/main/wrf.exe"
    ldd main/wrf.exe | grep -E "netcdf|pnetcdf|hdf5|mpi|omp" || true
    cat > "$PREFIX/wrf_env.sh" <<EOF
# source this before running WRF built by build_wrf.sh ($TOOLCHAIN)
export NETCDF=$NETCDF PNETCDF=$PNETCDF
export LD_LIBRARY_PATH=$NETCDF/lib:$PNETCDF/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
export OMP_STACKSIZE=\${OMP_STACKSIZE:-1G}
ulimit -s unlimited 2>/dev/null || true
EOF
    echo "environment: $PREFIX/wrf_env.sh"
}

# ── conus12km case ─────────────────────────────────────────────────────────
setup_case() {
    local tar=${1:-}
    [ -f "$tar" ] || die "usage: $0 case /path/to/v4.4_bench_conus12km.tar.gz
  (from https://www2.mmm.ucar.edu/wrf/users/benchmark/v44/benchdata_v44.html)"
    local W=$SRC_DIR/WRF RUN=$PREFIX/run_conus12km
    [ -x "$W/main/wrf.exe" ] || die "build WRF first ($0 wrf)"
    say "run directory $RUN"
    mkdir -p "$RUN"
    # the WRF run directory: tables and data files (symlinks), then the case
    for f in "$W"/run/*; do
        case $(basename "$f") in namelist.input|*.exe|README*) ;; *) ln -sf "$f" "$RUN/" ;; esac
    done
    ln -sf "$W/main/wrf.exe" "$RUN/wrf.exe"
    tar -xzf "$tar" -C "$RUN" --strip-components=1
    cd "$RUN"
    ls namelist.input* wrfinput_d01* wrfbdy_d01* wrfrst_d01* 2>/dev/null || true
    [ -f namelist.input ] || die "no namelist.input in the case; pick one of the namelist.input* files and copy it to $RUN/namelist.input"
    cp namelist.input namelist.input.orig
    # history through PnetCDF (io_form 11); restart and input keep their
    # format (the case files may be netCDF-4, which PnetCDF cannot read)
    if grep -qE "^\s*io_form_history" namelist.input; then
        sed -i -E 's/^(\s*io_form_history\s*=\s*)[0-9]+/\111/' namelist.input
    else
        sed -i -E 's/^(\s*&time_control.*)$/\1\n io_form_history = 11,/' namelist.input
    fi
    say "namelist (changes against namelist.input.orig):"
    diff namelist.input.orig namelist.input || true
    grep -E "^\s*(run_|start_|end_|time_step|history_interval|restart|io_form|e_we|e_sn|e_vert|numtiles|nproc_)" namelist.input
    for f in wrfinput_d01 wrfbdy_d01 wrfrst_d01*; do
        [ -f "$f" ] && printf '%-40s %s\n' "$f" "$(od -An -c -N4 "$f" | tr -d ' \n')"
    done
    echo "(CDF = classic netCDF, readable by PnetCDF; \\211HDF = netCDF-4)"
}

case $STEP in
  deps) build_deps ;;
  wrf)  build_wrf ;;
  case) setup_case "${2:-}" ;;
  all)  build_deps; build_wrf; [ -n "${2:-}" ] && setup_case "$2" ;;
  *) sed -n '2,30p' "$0"; exit 1 ;;
esac
say "done ($TOOLCHAIN, MARCH=$MARCH, $MPI_KIND)"
