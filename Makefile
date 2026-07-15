# miniprof Makefile
#
# Targets:
#   all            — build libminiprof.so and the bundled examples
#   lib            — build libminiprof.so only
#   examples       — build the examples via examples/Makefile
#   example        — build examples/heat2d (sample mode, default)
#   example-trace  — build examples/heat2d+trace (instrumented C binary)
#   example-f      — build examples/heat1d (Fortran sample-mode example)
#   example-f-trace— build examples/heat1d+trace (Fortran instrumented binary)
#   example-3d     — build examples/diffusion3d (3D anisotropic diffusion, 2D Cartesian MPI)
#   example-3d-trace— build examples/diffusion3d+trace (instrumented)
#   mpa-check      — build the standalone .mpa validator (no MPI needed)
#   run            — sampling run with 4 ranks (heat2d)
#   run-trace      — validate + trace run (heat2d)
#   run-f          — Fortran equivalent of run
#   run-f-trace    — Fortran equivalent of run-trace
#   run-3d         — sampling run with 4 ranks (diffusion3d)
#   run-3d-trace   — validate + trace run (diffusion3d)
#   mpa            — full two-pass workflow (heat2d, clean→build→sample→trace)
#   clean          — remove build artefacts
#
# Grid size override for diffusion3d:
#   make example-3d NX=512 NY=512 NZ=256 NSTEPS_3D=500

ifeq ($(origin CC), default)
CC := $(shell command -v mpicc 2>/dev/null || true)
ifeq ($(strip $(CC)),)
CC := /usr/mpi/gcc/openmpi-4.1.7a1/bin/mpicc
endif
ifeq ($(wildcard $(CC)),)
CC := cc
endif
endif

ifeq ($(origin FC), default)
FC := $(shell command -v mpifort 2>/dev/null || true)
ifeq ($(strip $(FC)),)
FC := /usr/mpi/gcc/openmpi-4.1.7a1/bin/mpifort
endif
ifeq ($(wildcard $(FC)),)
FC := gfortran
endif
endif

CC_HOST ?= cc
CFLAGS  := -O2 -g -Wall -Wextra \
           -fno-omit-frame-pointer \
           -fPIC \
           -I$(CURDIR)/include
FFLAGS  := -O0 -g -fPIC
LDFLAGS := -ldl -lm -Wl,-z,noexecstack

EXAMPLE_DIR := examples

# ── sources ──────────────────────────────────────────────────────────────────
# The library is mode-agnostic: trace-mode-only code (instrument.c,
# trace_store.c, mpi_trace.c, trace_aggregator.c) is always linked in but
# self-disables at runtime when MINIPROF_MODE != trace, so a single build
# of libminiprof.so supports both modes — only the *target* binary needs a
# different build flag (-finstrument-functions) to actually emit the
# function-entry/exit calls that trace mode consumes.
#
# miniprof_fortran.c (mpi_init_/mpi_init_thread_/mpi_finalize_) references
# pmpi_init_ etc., which only exist in libmpi_mpifh.so (linked by mpifort,
# not mpicc). That used to force it into its own object file in the static
# archive so plain C builds wouldn't drag in the dependency. As a shared
# library loaded via LD_PRELOAD, symbols resolve lazily at runtime instead:
# mpi_init_ only pulls in pmpi_init_ if something actually calls it, which
# only happens for Fortran applications, so there's no link-time failure
# for C codes and no need to keep the file separate anymore.
LIB_SRCS := src/profile_store.c    \
            src/path_store.c       \
            src/sampler.c          \
            src/sampler_hwc.c      \
            src/symbolizer.c       \
            src/aggregator.c       \
            src/mpa_writer.c       \
            src/trace_store.c      \
            src/instrument.c       \
            src/mpi_trace.c        \
            src/trace_aggregator.c \
            src/miniprof.c         \
            src/miniprof_fortran.c

LIB_OBJS := $(LIB_SRCS:.c=.o)
LIB      := libminiprof.so

EXAMPLE       := $(EXAMPLE_DIR)/heat2d
EXAMPLE_TRACE := $(EXAMPLE_DIR)/heat2d+trace
EXAMPLE_F     := $(EXAMPLE_DIR)/heat1d
EXAMPLE_F_TRACE := $(EXAMPLE_DIR)/heat1d+trace
EXAMPLE_3D    := $(EXAMPLE_DIR)/diffusion3d
EXAMPLE_3D_TRACE := $(EXAMPLE_DIR)/diffusion3d+trace

# ── rules ─────────────────────────────────────────────────────────────────────
.PHONY: all lib examples example example-trace example-f example-f-trace \
        example-3d example-3d-trace mpa-check \
        run run-trace run-f run-f-trace run-3d run-3d-trace mpa clean

all: lib examples

lib: $(LIB)

examples:
	$(MAKE) -C $(EXAMPLE_DIR)

$(LIB): $(LIB_OBJS)
	$(CC) -shared -fPIC -o $@ $^ $(LDFLAGS)
	@echo "  CC  $@  (shared library)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  CC  $<"

example: $(EXAMPLE)

$(EXAMPLE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) heat2d

# Tracing build: -finstrument-functions makes the compiler call
# __cyg_profile_func_enter/exit around every function in this file.
# Runtime filtering (instrument.c) keeps overhead proportional only to
# the functions actually named in the .mpa list, not to total call count.
example-trace: $(EXAMPLE_TRACE)

$(EXAMPLE_TRACE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) heat2d+trace

# Fortran example — heat1d.f90, the same two-pass workflow but for an
# MPI Fortran code, exercising the Fortran PMPI binding wrappers in
# src/miniprof_fortran.c and src/mpi_trace.c (mpi_init_, mpi_sendrecv_,
# mpi_allreduce_, etc.). Built at -O0 deliberately: gfortran inlines
# small internal `contains` subroutines very aggressively at -O2, which
# would remove them as separate traceable symbols entirely — see
# README.md's "Fortran support" section.
example-f: $(EXAMPLE_F)

$(EXAMPLE_F): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) heat1d

example-f-trace: $(EXAMPLE_F_TRACE)

$(EXAMPLE_F_TRACE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) heat1d+trace

# ── 3D anisotropic diffusion with 2D Cartesian MPI topology ─────────────────
# diffusion3d.c is the main scientific benchmark, a physically meaningful
# 3D heat equation in a heterogeneous medium. It exercises the profiler
# with multiple clearly distinct hot spots:
#   compute_diffusion()   — inner 7-point stencil with variable conductivity
#   update_source()       — volumetric heat source evaluation
#   halo_exchange_x/y()  — MPI face exchange in X and Y separately
#   apply_bc()            — Dirichlet BC on Z planes
#   compute_diagnostics() — MPI_Allreduce for global L2/min/max
#
# Grid size is controlled by compile-time defines:
#   make example-3d NX=128 NY=128 NZ=64 NSTEPS=100
example-3d: $(EXAMPLE_3D)

NX     ?= 256
NY     ?= 256
NZ     ?= 128
NSTEPS_3D ?= 200

$(EXAMPLE_3D): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) diffusion3d NX=$(NX) NY=$(NY) NZ=$(NZ) NSTEPS_3D=$(NSTEPS_3D)

example-3d-trace: $(EXAMPLE_3D_TRACE)

$(EXAMPLE_3D_TRACE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) diffusion3d+trace NX=$(NX) NY=$(NY) NZ=$(NZ) NSTEPS_3D=$(NSTEPS_3D)

# Standalone validator — deliberately NOT linked against libminiprof or
# MPI at all (it just runs `nm` on a binary and cross-checks a .mpa file),
# so it builds fast and runs in under a second, fit for a pre-submission
# check in a job script or CI pipeline before burning cluster allocation.
mpa-check: src/mpa_check.c
	$(CC_HOST) -O2 -Wall -Wextra -o $@ $<
	@echo "  CC  $@  (standalone .mpa validator, no MPI/library deps)"

# ── run: sampling pass ──────────────────────────────────────────────────────
NRANKS ?= 4

run: $(EXAMPLE)
	@echo "Running heat2d (SAMPLE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=sample \
	MINIPROF_INTERVAL_MS=5 \
	MINIPROF_DEPTH=10 \
	MINIPROF_OUTPUT=miniprof_report.txt \
	MINIPROF_MPA_OUTPUT=miniprof.mpa \
	MINIPROF_MPA_THRESHOLD=1.0 \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE)
	@echo ""
	@echo "=== miniprof sampling report ==="
	@cat miniprof_report.txt
	@echo ""
	@echo "=== miniprof.mpa suggestions ==="
	@cat miniprof.mpa

# ── run-trace: tracing pass, using the .mpa from `run` ──────────────────────
# mpa-check runs first as a fast (<1s) pre-flight gate: if any function in
# the .mpa file won't resolve against the trace binary, we abort here
# rather than burning an N-rank mpirun allocation only to discover a typo
# afterward in the trace report.
run-trace: $(EXAMPLE_TRACE) mpa-check
	@echo "Validating miniprof.mpa against $(EXAMPLE_TRACE)..."
	./mpa-check $(EXAMPLE_TRACE) miniprof.mpa
	@echo ""
	@echo "Running heat2d+trace (TRACE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=trace \
	MINIPROF_TRACE_FUNCS=miniprof.mpa \
	MINIPROF_TRACE_OUTPUT=miniprof_trace_report.txt \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE_TRACE)
	@echo ""
	@echo "=== miniprof tracing report ==="
	@cat miniprof_trace_report.txt

# ── mpa: full sample, validate, and trace workflow ──────────────────────────
mpa: clean all example-trace run run-trace

# ── Fortran run targets (same workflow, Fortran binary) ─────────────────────
run-f: $(EXAMPLE_F)
	@echo "Running heat1d (Fortran, SAMPLE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=sample \
	MINIPROF_INTERVAL_MS=5 \
	MINIPROF_DEPTH=10 \
	MINIPROF_OUTPUT=miniprof_report_f.txt \
	MINIPROF_MPA_OUTPUT=miniprof_f.mpa \
	MINIPROF_MPA_THRESHOLD=1.0 \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE_F)
	@echo ""
	@echo "=== miniprof sampling report (Fortran) ==="
	@cat miniprof_report_f.txt
	@echo ""
	@echo "=== miniprof_f.mpa suggestions ==="
	@cat miniprof_f.mpa

run-f-trace: $(EXAMPLE_F_TRACE) mpa-check
	@echo "Validating miniprof_f.mpa against $(EXAMPLE_F_TRACE)..."
	./mpa-check $(EXAMPLE_F_TRACE) miniprof_f.mpa
	@echo ""
	@echo "Running heat1d+trace (Fortran, TRACE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=trace \
	MINIPROF_TRACE_FUNCS=miniprof_f.mpa \
	MINIPROF_TRACE_OUTPUT=miniprof_trace_report_f.txt \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE_F_TRACE)
	@echo ""
	@echo "=== miniprof tracing report (Fortran) ==="
	@cat miniprof_trace_report_f.txt

# ── run-3d: 3D diffusion, sampling pass ─────────────────────────────────────
run-3d: $(EXAMPLE_3D)
	@echo "Running diffusion3d (SAMPLE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=sample \
	MINIPROF_INTERVAL_MS=10 \
	MINIPROF_DEPTH=10 \
	MINIPROF_OUTPUT=miniprof_report_3d.txt \
	MINIPROF_MPA_OUTPUT=miniprof_3d.mpa \
	MINIPROF_MPA_THRESHOLD=0.5 \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE_3D)
	@echo ""
	@echo "=== miniprof sampling report (3D diffusion) ==="
	@cat miniprof_report_3d.txt
	@echo ""
	@echo "=== miniprof_3d.mpa suggestions ==="
	@cat miniprof_3d.mpa

# ── run-3d-trace: 3D diffusion, tracing pass ────────────────────────────────
run-3d-trace: $(EXAMPLE_3D_TRACE) mpa-check
	@echo "Validating miniprof_3d.mpa against $(EXAMPLE_3D_TRACE)..."
	./mpa-check $(EXAMPLE_3D_TRACE) miniprof_3d.mpa
	@echo ""
	@echo "Running diffusion3d+trace (TRACE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=trace \
	MINIPROF_TRACE_FUNCS=miniprof_3d.mpa \
	MINIPROF_TRACE_OUTPUT=miniprof_trace_report_3d.txt \
	MINIPROF_VERBOSE=1 \
	mpirun -n $(NRANKS) ./$(EXAMPLE_3D_TRACE)
	@echo ""
	@echo "=== miniprof tracing report (3D diffusion) ==="
	@cat miniprof_trace_report_3d.txt

# ── clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f $(LIB_OBJS) $(LIB) $(EXAMPLE) $(EXAMPLE_TRACE) \
	      $(EXAMPLE_F) $(EXAMPLE_F_TRACE) \
	      $(EXAMPLE_3D) $(EXAMPLE_3D_TRACE) mpa-check \
	      miniprof_report.txt miniprof.mpa miniprof_trace_report.txt \
	      miniprof_report_f.txt miniprof_f.mpa miniprof_trace_report_f.txt \
	      miniprof_report_3d.txt miniprof_3d.mpa miniprof_trace_report_3d.txt
