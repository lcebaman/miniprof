# Example build rules for miniprof
#
# This Makefile keeps example binaries and their build flags local to the
# examples directory while still linking against the library built from the
# repository root.

ROOT := ..
LIB := $(ROOT)/libminiprof.so

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

.PHONY: all clean heat2d heat2d+trace heat1d heat1d+trace diffusion3d diffusion3d+trace hwc_validate

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

clean:
	rm -f $(EXAMPLES) hwc_validate
