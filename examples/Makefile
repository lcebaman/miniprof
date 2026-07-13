# Example build rules for miniprof
#
# This Makefile keeps example binaries and their build flags local to the
# examples directory while still linking against the library built from the
# repository root.

ROOT := ..
LIB := $(ROOT)/libminiprof.a

$(LIB):
	$(MAKE) -C $(ROOT) lib

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
LDFLAGS := -ldl -lm -Wl,-z,noexecstack

NX     ?= 256
NY     ?= 256
NZ     ?= 128
NSTEPS_3D ?= 200

EXAMPLES := heat2d heat2d+trace heat1d heat1d+trace diffusion3d diffusion3d+trace

.PHONY: all clean heat2d heat2d+trace heat1d heat1d+trace diffusion3d diffusion3d+trace

all: $(EXAMPLES)

heat2d: heat2d.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(ROOT) -lminiprof $(LDFLAGS) -o $@

heat2d+trace: heat2d.c $(LIB)
	$(CC) $(CFLAGS) -finstrument-functions $< -L$(ROOT) -lminiprof $(LDFLAGS) -o $@

heat1d: heat1d.f90 $(LIB)
	$(FC) $(FFLAGS) $< -L$(ROOT) -lminiprof $(LDFLAGS) -o $@

heat1d+trace: heat1d.f90 $(LIB)
	$(FC) $(FFLAGS) -finstrument-functions $< -L$(ROOT) -lminiprof $(LDFLAGS) -o $@

diffusion3d: diffusion3d.c $(LIB)
	$(CC) $(CFLAGS) -DNX_GLOBAL=$(NX) -DNY_GLOBAL=$(NY) -DNZ_GLOBAL=$(NZ) -DNSTEPS=$(NSTEPS_3D) $< -L$(ROOT) -lminiprof $(LDFLAGS) -lm -o $@

diffusion3d+trace: diffusion3d.c $(LIB)
	$(CC) $(CFLAGS) -DNX_GLOBAL=$(NX) -DNY_GLOBAL=$(NY) -DNZ_GLOBAL=$(NZ) -DNSTEPS=$(NSTEPS_3D) -finstrument-functions $< -L$(ROOT) -lminiprof $(LDFLAGS) -lm -o $@

clean:
	rm -f $(EXAMPLES)
