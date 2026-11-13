# Example build rules for miniprof
#
# This Makefile keeps example binaries and their build flags local to the
# examples directory while still linking against the library built from the
# repository root.

ROOT := ..
LIB := $(ROOT)/libminiprof.so

# The $(LIB) rule below is the first rule in this file, so without this
# it would silently become the default goal — `make -C examples` (and so
# the root `make examples`) would rebuild nothing but the library.
.DEFAULT_GOAL := all

$(LIB):
	$(MAKE) -C $(ROOT) lib

# Examples link against the .so directly (rather than relying on
# LD_PRELOAD) to demonstrate the explicit-link workflow; the rpath lets
# the resulting binaries find libminiprof.so without LD_LIBRARY_PATH.
RPATH := -Wl,-rpath,$(abspath $(ROOT))

ifeq ($(origin CC), default)
CC := $(shell command -v mpicc 2>/dev/null || true)
ifeq ($(strip $(CC)),)
CC := mpicc
endif
ifeq ($(wildcard $(CC)),)
CC := mpicc
endif
endif

ifeq ($(origin FC), default)
FC := $(shell command -v mpifort 2>/dev/null || true)
ifeq ($(strip $(FC)),)
FC := mpifort
endif
ifeq ($(wildcard $(FC)),)
FC := mpifort
endif
endif

CC_HOST ?= mpicc
CFLAGS  := -O2 -g -Wall -Wextra -fno-omit-frame-pointer -I$(ROOT)/include
FFLAGS  := -O0 -g
# --allow-shlib-undefined: libminiprof.so references pmpi_init_/
# pmpi_init_thread_/pmpi_finalize_ (Fortran PMPI bindings, only present in
# libmpi_mpifh.so, which mpifort links but mpicc does not). Those stay
# unresolved in C builds by design — see the sources comment in the root
# Makefile — but some linker/toolchain defaults treat an indirect shared
# library's undefined symbols as a hard link error unless told they're
# expected to resolve at load time instead.
LDFLAGS := -ldl -lm -Wl,-z,noexecstack -Wl,--allow-shlib-undefined

NX     ?= 256
NY     ?= 256
NZ     ?= 128
NSTEPS_3D ?= 200

EXAMPLES := heat2d heat2d+trace heat1d heat1d+trace diffusion3d diffusion3d+trace

.PHONY: all clean heat2d heat2d+trace heat1d heat1d+trace diffusion3d diffusion3d+trace hwc_validate hwc_validate_omp hwc_validate_smear mpi_ext_validate mpi_ext_validate_f imbalance_validate fortran_sentinels counters_validate mpi_percall omp_validate omp_idle_validate topdown_validate membw_validate roofline_fma

all: $(EXAMPLES)

heat2d: heat2d.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

heat2d+trace: heat2d.c $(LIB)
	$(CC) $(CFLAGS) -finstrument-functions $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

heat1d: heat1d.f90 $(LIB)
	$(FC) $(FFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

heat1d+trace: heat1d.f90 $(LIB)
	$(FC) $(FFLAGS) -finstrument-functions $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

diffusion3d: diffusion3d.c $(LIB)
	$(CC) $(CFLAGS) -DNX_GLOBAL=$(NX) -DNY_GLOBAL=$(NY) -DNZ_GLOBAL=$(NZ) -DNSTEPS=$(NSTEPS_3D) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -lm -o $@

diffusion3d+trace: diffusion3d.c $(LIB)
	$(CC) $(CFLAGS) -DNX_GLOBAL=$(NX) -DNY_GLOBAL=$(NY) -DNZ_GLOBAL=$(NZ) -DNSTEPS=$(NSTEPS_3D) -finstrument-functions $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -lm -o $@

# Not part of `all` — a manual validator for Table 2 (hardware counter
# characterisation), not a demo. See `make run-hwc-validate` at the repo
# root.
hwc_validate: hwc_validate.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# MPI+OpenMP variant — validates PER-THREAD hardware counters
# specifically (see hwc_validate_omp.c). Needs -fopenmp and
# MPI_THREAD_FUNNELED support from the MPI implementation.
hwc_validate_omp: hwc_validate_omp.c $(LIB)
	$(CC) $(CFLAGS) -fopenmp $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Negative control for the Table 2 "interval smearing" self-diagnostic
# (see hwc_validate_smear.c) — many tiny functions, so the warning
# should fire, unlike hwc_validate/hwc_validate_omp's long-running ones.
hwc_validate_smear: hwc_validate_smear.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Validator for the MPI_Test/Wait family + one-sided RMA wrappers in
# mpi_trace.c (see mpi_ext_validate.c/.f90). Needs >= 2 ranks.
mpi_ext_validate: mpi_ext_validate.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

mpi_ext_validate_f: mpi_ext_validate.f90 $(LIB)
	$(FC) $(FFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Known-answer workload for snapshot mode: injected per-rank compute
# imbalance plus a fixed POSIX and MPI-IO byte volume (see the file header).
imbalance_validate: imbalance_validate.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Per-call cost of the MPI wrappers (off vs snapshot, rdpmc vs read()).
mpi_percall: mpi_percall.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Known-FLOP workload for snapshot-mode hardware counters (regions,
# worker threads, MPI phase) — see the file header.
counters_validate: counters_validate.c $(LIB)
	$(CC) $(CFLAGS) -pthread $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Known DRAM traffic for the memory block (P5): a triad, see the header.
membw_validate: membw_validate.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Compute-bound FMA workload for the roofline check (P6).
roofline_fma: roofline_fma.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Known-shape top-down workload (P4): one region per top-down category.
topdown_validate: topdown_validate.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Known-answer MPI+OpenMP workload for snapshot-mode OMPT (P2). OMPT needs
# an LLVM-family OpenMP runtime (clang, AOCC, ICX); with GCC's libgomp it
# builds and runs, and the validator reports SKIP.
OMP_FLAG ?= -fopenmp
omp_validate: omp_validate.c $(LIB)
	$(CC) $(CFLAGS) $(OMP_FLAG) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

omp_idle_validate: omp_idle_validate.c $(LIB)
	$(CC) $(CFLAGS) $(OMP_FLAG) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

# Regression test for Fortran MPI sentinels (MPI_IN_PLACE,
# MPI_STATUS(ES)_IGNORE) passing through the Fortran wrappers intact.
fortran_sentinels: fortran_sentinels.f90 $(LIB)
	$(FC) $(FFLAGS) $< -L$(ROOT) -lminiprof $(RPATH) $(LDFLAGS) -o $@

clean:
	rm -f $(EXAMPLES) hwc_validate hwc_validate_omp hwc_validate_smear \
	      mpi_ext_validate mpi_ext_validate_f imbalance_validate fortran_sentinels \
	      counters_validate mpi_percall omp_validate omp_idle_validate topdown_validate membw_validate roofline_fma
