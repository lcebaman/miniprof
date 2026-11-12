#!/usr/bin/env bash
# build_lammps.sh — build LAMMPS (MPI + OpenMP) for the miniprof validation.
#
#   tools/lammps/build_lammps.sh            clone the stable release and build
#
# Packages: OPENMP, MOLECULE, KSPACE, RIGID — what bench/in.rhodo needs
# (CHARMM force field, PPPM long-range electrostatics, SHAKE). FFT: FFTW3 if
# found, else LAMMPS's own KISS FFT; no other dependencies.
#
# Environment (all optional):
#   TOOLCHAIN  aocc | intel | gcc | llvm (default: aocc, intel, gcc, first found).
#              aocc, intel or llvm for miniprof: their OpenMP runtimes support
#              OMPT; GCC's libgomp does not.
#   PREFIX     build directory (default ./lammps-build)
#   JOBS       parallel make jobs (default nproc)
#   MARCH      target CPU (default znver4; Intel default core-avx2)
#   LAMMPS_BRANCH  default stable
#   SRC        an existing LAMMPS checkout (skips the clone)
# The binary is $PREFIX/build/lmp; the MPI wrappers on PATH must be built with
# the same C++ ABI (any compiler is fine for C++ in practice).
set -euo pipefail
PREFIX=$(mkdir -p "${PREFIX:-$PWD/lammps-build}" && cd "${PREFIX:-$PWD/lammps-build}" && pwd)
JOBS=${JOBS:-$(nproc)}
BRANCH=${LAMMPS_BRANCH:-stable}
SRC=${SRC:-$PREFIX/lammps}
say() { printf '\n==> %s\n' "$*"; }
die() { printf '\n!! %s\n' "$*" >&2; exit 1; }

TOOLCHAIN=${TOOLCHAIN:-auto}
if [ "$TOOLCHAIN" = auto ]; then
    if clang --version 2>/dev/null | grep -qi aocc; then TOOLCHAIN=aocc
    elif command -v icpx >/dev/null; then TOOLCHAIN=intel
    else TOOLCHAIN=gcc; fi
fi
case $TOOLCHAIN in
  aocc|llvm) CC=${CC_OVERRIDE:-clang}; CXX=${CXX_OVERRIDE:-clang++}; MARCH=${MARCH:-znver4} ;;
  intel)     CC=icx;   CXX=icpx;    MARCH=${MARCH:-core-avx2} ;;
  gcc)       CC=gcc;   CXX=g++;     MARCH=${MARCH:-znver4} ;;
  *) die "TOOLCHAIN must be aocc, intel, gcc or llvm" ;;
esac
for c in "$CXX" mpicxx cmake make git; do command -v "$c" >/dev/null || die "$c not on PATH"; done
export OMPI_CXX=$CXX OMPI_CC=$CC MPICH_CXX=$CXX MPICH_CC=$CC I_MPI_CXX=$CXX I_MPI_CC=$CC

if [ ! -d "$SRC/cmake" ]; then
    say "cloning LAMMPS ($BRANCH)"
    git clone -q --depth 1 -b "$BRANCH" https://github.com/lammps/lammps "$SRC" \
        || die "clone failed: put a LAMMPS checkout in $SRC (or set SRC)"
fi
say "LAMMPS $(grep -m1 LAMMPS_VERSION "$SRC/src/version.h" | cut -d'"' -f2), $TOOLCHAIN ($($CXX --version | head -1)), -march=$MARCH"

FFT=KISS; [ -n "${FFTW_HOME:-}${FFTW3_DIR:-}" ] || pkg-config --exists fftw3 2>/dev/null && FFT=FFTW3
mkdir -p "$PREFIX/build"
cmake -S "$SRC/cmake" -B "$PREFIX/build" \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_C_COMPILER="$CC" -D CMAKE_CXX_COMPILER="$CXX" \
    -D CMAKE_CXX_FLAGS_RELEASE="-O3 -march=$MARCH -DNDEBUG" \
    -D BUILD_MPI=on -D BUILD_OMP=on -D LAMMPS_MACHINE= \
    -D PKG_OPENMP=on -D PKG_MOLECULE=on -D PKG_KSPACE=on -D PKG_RIGID=on \
    -D FFT="$FFT" -D BUILD_SHARED_LIBS=off -D WITH_JPEG=off -D WITH_PNG=off \
    -D WITH_FFMPEG=off -D WITH_GZIP=off -D DOWNLOAD_POTENTIALS=off \
    > "$PREFIX/cmake.log" 2>&1 || { tail -30 "$PREFIX/cmake.log"; die "cmake failed: $PREFIX/cmake.log"; }
grep -E "OpenMP|MPI|FFT|Enabled packages" "$PREFIX/cmake.log" | grep -v "^--   " | head -12 || true
say "make -j $JOBS  [log: $PREFIX/make.log, a few minutes]"
cmake --build "$PREFIX/build" -j "$JOBS" > "$PREFIX/make.log" 2>&1 \
    || { grep -m10 -n "error" "$PREFIX/make.log"; die "build failed: $PREFIX/make.log"; }
"$PREFIX/build/lmp" -h | awk '/^(OS|Compiler|C\+\+ standard):/ {print}
    /^Installed packages/ {p = 1; print; next} p && NF {print; n++} p && !NF && n {exit}'
ldd "$PREFIX/build/lmp" | grep -E "mpi|omp|fftw" || true
say "built $PREFIX/build/lmp"
echo "bench inputs: $SRC/bench (in.rhodo.scaled + data.rhodo)"
