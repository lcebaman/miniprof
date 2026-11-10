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
#   example-hwc-validate — build examples/hwc_validate (Table 2 sanity check)
#   example-hwc-validate-omp — build examples/hwc_validate_omp (per-thread
#                      counter sanity check, MPI+OpenMP)
#   mpa-check      — build the standalone .mpa validator (no MPI needed)
#   run            — sampling run with 4 ranks (heat2d)
#   run-trace      — validate + trace run (heat2d)
#   run-f          — Fortran equivalent of run
#   run-f-trace    — Fortran equivalent of run-trace
#   run-3d         — sampling run with 4 ranks (diffusion3d)
#   run-3d-trace   — validate + trace run (diffusion3d)
#   run-hwc-validate — sanity-check Table 2 (hardware counter characterisation)
#                      against a known compute-bound / memory-bound kernel pair
#   run-hwc-validate-omp — same, but with the two kernels running
#                      concurrently on separate OpenMP threads, to check
#                      counters aren't cross-attributed between threads
#   run-hwc-validate-smear — negative control for the Table 2 interval-
#                      smearing warning: many tiny functions, so the
#                      warning should fire (opposite of the other two)
#   example-mpi-ext-validate — build mpi_ext_validate(_f): MPI_Test/Wait
#                      family + one-sided RMA wrapper sanity check, C+Fortran
#   run-ext-trace  — trace-mode check that every new MPI wrapper above
#                      is actually recorded, for both languages
#   test-units     — MPI-free unit tests (uarch table, JSON writer)
#   run-3d-snapshot — snapshot mode on diffusion3d + JSON structural check
#   overhead-3d    — wall-clock overhead of snapshot vs off (informational)
#   run-imbalance-validate — snapshot known-answer test: injected imbalance,
#                      trace-mode cross-check, exact I/O byte counts
#   run-omp-validate     — snapshot OpenMP (OMPT): per-region imbalance,
#                          team size, serial time and lock wait against
#                          injected values; SKIP under GCC libgomp
#   test-ompt-abi        — src/ompt_abi.h against the real <omp-tools.h>
#   run-counters-validate — snapshot hardware counters: exact region FLOPs,
#                      threads, compute/MPI split (SKIP without a PMU)
#   run-fortran-sentinels — Fortran MPI_IN_PLACE/STATUS_IGNORE results
#                      correct in every mode, call counts not doubled
#   check          — full test suite, one pass/fail exit code (what CI runs)
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
            src/io_trace.c         \
            src/counters.c         \
            src/ompt.c             \
            src/json_writer.c      \
            src/sysinfo.c          \
            src/snapshot.c         \
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
EXAMPLE_HWC_VALIDATE := $(EXAMPLE_DIR)/hwc_validate
EXAMPLE_HWC_VALIDATE_OMP := $(EXAMPLE_DIR)/hwc_validate_omp
EXAMPLE_HWC_VALIDATE_SMEAR := $(EXAMPLE_DIR)/hwc_validate_smear
EXAMPLE_MPI_EXT_VALIDATE := $(EXAMPLE_DIR)/mpi_ext_validate
EXAMPLE_MPI_EXT_VALIDATE_F := $(EXAMPLE_DIR)/mpi_ext_validate_f

# ── rules ─────────────────────────────────────────────────────────────────────
.PHONY: all lib examples example example-trace example-f example-f-trace \
        example-3d example-3d-trace example-hwc-validate \
        example-hwc-validate-omp example-hwc-validate-smear \
        example-mpi-ext-validate mpa-check \
        run run-trace run-f run-f-trace run-3d run-3d-trace \
        run-hwc-validate run-hwc-validate-omp run-hwc-validate-smear \
        run-ext-trace run-3d-snapshot test-units overhead-3d \
        run-imbalance-validate run-fortran-sentinels run-counters-validate \
        run-omp-validate test-ompt-abi \
        mpa check clean

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

# hwc_validate — sanity check for Table 2 (hardware counter
# characterisation): one kernel deliberately compute-bound (independent
# FP chains, tiny working set) and one deliberately memory-bound
# (pointer-chase over a buffer many times larger than LLC). Not part of
# `make all` — a manual/CI check, see run-hwc-validate below.
example-hwc-validate: $(EXAMPLE_HWC_VALIDATE)

$(EXAMPLE_HWC_VALIDATE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) hwc_validate

# hwc_validate_omp — same two kernels, but run concurrently on separate
# OpenMP threads within one MPI rank, specifically to check that
# per-thread counter groups aren't cross-attributed (see
# sampler_hwc.c's file header for the bug this guards against).
example-hwc-validate-omp: $(EXAMPLE_HWC_VALIDATE_OMP)

$(EXAMPLE_HWC_VALIDATE_OMP): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) hwc_validate_omp

# hwc_validate_smear — negative control for Table 2's interval-smearing
# warning (see the detector in aggregator.c): 20 tiny, near-instantaneous
# functions round-robin-called for several seconds, guaranteeing many
# calls per sampling interval. Unlike the two validators above, this one
# is EXPECTED to trigger the warning.
example-hwc-validate-smear: $(EXAMPLE_HWC_VALIDATE_SMEAR)

$(EXAMPLE_HWC_VALIDATE_SMEAR): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) hwc_validate_smear

# mpi_ext_validate(_f) — validates the MPI_Test/Wait family (Test,
# Testany, Testall, Waitany, Waitall) and one-sided RMA (Win_create,
# Win_fence, Put, Get, Win_free) wrappers added to mpi_trace.c, in both
# C and Fortran. See run-ext-trace below for the actual check.
example-mpi-ext-validate: $(EXAMPLE_MPI_EXT_VALIDATE) $(EXAMPLE_MPI_EXT_VALIDATE_F)

$(EXAMPLE_MPI_EXT_VALIDATE): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) mpi_ext_validate

$(EXAMPLE_MPI_EXT_VALIDATE_F): $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) mpi_ext_validate_f

# Standalone validator — deliberately NOT linked against libminiprof or
# MPI at all (it just runs `nm` on a binary and cross-checks a .mpa file),
# so it builds fast and runs in under a second, fit for a pre-submission
# check in a job script or CI pipeline before burning cluster allocation.
mpa-check: src/mpa_check.c
	$(CC_HOST) -O2 -Wall -Wextra -o $@ $<
	@echo "  CC  $@  (standalone .mpa validator, no MPI/library deps)"

# ── run: sampling pass ──────────────────────────────────────────────────────
NRANKS ?= 4
# Extra flags passed to every mpirun invocation below — empty by default, so
# nothing changes for interactive/HPC use. CI passes --oversubscribe here,
# since hosted runners commonly have fewer cores than NRANKS requests and
# Open MPI otherwise refuses to place more ranks than physical slots.
MPIRUN_FLAGS ?=

run: $(EXAMPLE)
	@echo "Running heat2d (SAMPLE mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=sample \
	MINIPROF_INTERVAL_MS=5 \
	MINIPROF_DEPTH=10 \
	MINIPROF_OUTPUT=miniprof_report.txt \
	MINIPROF_MPA_OUTPUT=miniprof.mpa \
	MINIPROF_MPA_THRESHOLD=1.0 \
	MINIPROF_VERBOSE=1 \
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE)
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
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_TRACE)
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
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_F)
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
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_F_TRACE)
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
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_3D)
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
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_3D_TRACE)
	@echo ""
	@echo "=== miniprof tracing report (3D diffusion) ==="
	@cat miniprof_trace_report_3d.txt

# ── run-hwc-validate: Table 2 sanity check ──────────────────────────────────
# Single rank is deliberate: this checks classification correctness, not
# parallel behaviour, and per-rank aggregation would only dilute the
# signal. ~6s wall time (two 3s kernels). SKIPs (exit 0) rather than
# FAILs when hardware counters aren't available on this system, since
# that's an environment property, not a bug.
run-hwc-validate: $(EXAMPLE_HWC_VALIDATE)
	@echo "Running hwc_validate (SAMPLE mode) with 1 MPI rank..."
	MINIPROF_MODE=sample \
	MINIPROF_OUTPUT=miniprof_hwc_validate_report.txt \
	MINIPROF_MPA_OUTPUT=miniprof_hwc_validate.mpa \
	mpirun $(MPIRUN_FLAGS) -n 1 ./$(EXAMPLE_HWC_VALIDATE)
	@echo ""
	@if grep -q "Hardware counters not available" miniprof_hwc_validate_report.txt; then \
		echo "SKIP: hardware counters unavailable on this system — nothing to validate."; \
	else \
		echo "=== Table 2 ==="; \
		sed -n '/Table 2:/,/^$$/p' miniprof_hwc_validate_report.txt; \
		echo ""; \
		FAIL=0; \
		grep "compute_bound_kernel" miniprof_hwc_validate_report.txt | grep -q "COMPUTE-BOUND" \
			&& echo "PASS: compute_bound_kernel classified COMPUTE-BOUND" \
			|| { echo "FAIL: compute_bound_kernel NOT classified COMPUTE-BOUND"; FAIL=1; }; \
		grep "memory_bound_kernel" miniprof_hwc_validate_report.txt | grep -q "MEMORY-BOUND" \
			&& echo "PASS: memory_bound_kernel classified MEMORY-BOUND" \
			|| { echo "FAIL: memory_bound_kernel NOT classified MEMORY-BOUND"; FAIL=1; }; \
		exit $$FAIL; \
	fi

# ── run-hwc-validate-omp: per-thread counter sanity check ───────────────────
# Same idea as run-hwc-validate, but the two kernels run CONCURRENTLY on
# separate OpenMP threads (see hwc_validate_omp.c) instead of
# sequentially on one. This specifically catches counter groups being
# opened once and read from every thread regardless of which thread is
# actually executing — see sampler_hwc.c's file header. ~4s wall time.
run-hwc-validate-omp: $(EXAMPLE_HWC_VALIDATE_OMP)
	@echo "Running hwc_validate_omp (SAMPLE mode) with 1 MPI rank, 2 OpenMP threads..."
	MINIPROF_MODE=sample \
	MINIPROF_OUTPUT=miniprof_hwc_validate_omp_report.txt \
	MINIPROF_MPA_OUTPUT=miniprof_hwc_validate_omp.mpa \
	mpirun $(MPIRUN_FLAGS) -n 1 ./$(EXAMPLE_HWC_VALIDATE_OMP)
	@echo ""
	@if grep -q "Hardware counters not available" miniprof_hwc_validate_omp_report.txt; then \
		echo "SKIP: hardware counters unavailable on this system — nothing to validate."; \
	else \
		echo "=== Table 2 ==="; \
		sed -n '/Table 2:/,/^$$/p' miniprof_hwc_validate_omp_report.txt; \
		echo ""; \
		FAIL=0; \
		grep "compute_bound_kernel" miniprof_hwc_validate_omp_report.txt | grep -q "COMPUTE-BOUND" \
			&& echo "PASS: compute_bound_kernel (thread 0) classified COMPUTE-BOUND" \
			|| { echo "FAIL: compute_bound_kernel NOT classified COMPUTE-BOUND"; FAIL=1; }; \
		grep "memory_bound_kernel" miniprof_hwc_validate_omp_report.txt | grep -q "MEMORY-BOUND" \
			&& echo "PASS: memory_bound_kernel (thread 1, concurrent) classified MEMORY-BOUND" \
			|| { echo "FAIL: memory_bound_kernel NOT classified MEMORY-BOUND"; FAIL=1; }; \
		exit $$FAIL; \
	fi

# ── run-hwc-validate-smear: interval-smearing warning negative control ──────
# Opposite check from the two targets above: this workload is EXPECTED
# to trigger Table 2's smearing warning (many tiny functions called far
# more often than the sampling interval), so PASS here means the
# warning appears, not that classification was correct. ~5s wall time.
run-hwc-validate-smear: $(EXAMPLE_HWC_VALIDATE_SMEAR)
	@echo "Running hwc_validate_smear (SAMPLE mode) with 1 MPI rank..."
	MINIPROF_MODE=sample \
	MINIPROF_OUTPUT=miniprof_hwc_validate_smear_report.txt \
	MINIPROF_MPA_OUTPUT=miniprof_hwc_validate_smear.mpa \
	mpirun $(MPIRUN_FLAGS) -n 1 ./$(EXAMPLE_HWC_VALIDATE_SMEAR)
	@echo ""
	@if grep -q "Hardware counters not available" miniprof_hwc_validate_smear_report.txt; then \
		echo "SKIP: hardware counters unavailable on this system — nothing to validate."; \
	else \
		sed -n '/Table 2:/,/^ PER-RANK/p' miniprof_hwc_validate_smear_report.txt; \
		grep -q "WARNING.*interval smearing" miniprof_hwc_validate_smear_report.txt \
			&& echo "PASS: interval-smearing warning fired as expected" \
			|| { echo "FAIL: smearing warning did NOT fire — detector may have regressed"; exit 1; }; \
	fi

# ── run-ext-trace: MPI_Test/Wait family + RMA wrapper sanity check ──────────
# Runs mpi_ext_validate (C) and mpi_ext_validate_f (Fortran) under TRACE
# mode and asserts every new wrapper actually shows up in Table 2 (MPI
# Call Tracing) with a nonzero call count, for both languages. The
# binaries themselves also self-check correctness (wrong RMA/request
# data aborts with a nonzero exit before miniprof even writes a report),
# so this target catches both "does the wrapper exist" and "does the
# underlying MPI call still behave correctly through the wrapper".
# MINIPROF_TRACE_FUNCS=/dev/null is deliberate: this only tests MPI call
# tracing (independent of the user-function trace list), not Table 1.
run-ext-trace: $(EXAMPLE_MPI_EXT_VALIDATE) $(EXAMPLE_MPI_EXT_VALIDATE_F)
	@echo "Running mpi_ext_validate (C) under TRACE mode, $(NRANKS) ranks..."
	MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=/dev/null \
	MINIPROF_TRACE_OUTPUT=miniprof_ext_trace_c.txt \
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_MPI_EXT_VALIDATE)
	@echo "Running mpi_ext_validate_f (Fortran) under TRACE mode, $(NRANKS) ranks..."
	MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=/dev/null \
	MINIPROF_TRACE_OUTPUT=miniprof_ext_trace_f.txt \
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_MPI_EXT_VALIDATE_F)
	@echo ""
	@FAIL=0; \
	for fn in MPI_Test MPI_Testany MPI_Waitall MPI_Waitany \
	          MPI_Win_create MPI_Win_fence MPI_Put MPI_Get MPI_Win_free; do \
		for f in miniprof_ext_trace_c.txt miniprof_ext_trace_f.txt; do \
			grep -q "  $$fn$$" $$f \
				&& echo "PASS: $$fn traced in $$f" \
				|| { echo "FAIL: $$fn missing from $$f"; FAIL=1; }; \
		done; \
	done; \
	exit $$FAIL

# ── test-units: MPI-free unit tests ──────────────────────────────────────────
# Compiled with the host compiler straight from the sources it tests (no
# libminiprof.so, no MPI), so it runs anywhere in well under a second. The
# JSON it writes is re-parsed by Python's json module as an independent
# check that the writer's output is valid JSON, not just self-consistent.
tests/test_units: tests/test_units.c src/sysinfo.c src/json_writer.c \
                  src/sysinfo.h src/json_writer.h
	$(CC_HOST) -O2 -g -Wall -Wextra -o $@ tests/test_units.c src/sysinfo.c src/json_writer.c -lm

test-units: tests/test_units
	./tests/test_units tests/test_units_out.json
	python3 tests/check_units_json.py tests/test_units_out.json

# ── run-3d-snapshot: snapshot mode + JSON structural check ──────────────────
# No rebuild needed — the normal diffusion3d binary, MINIPROF_MODE=snapshot.
# SNAPSHOT_MAX_OVERHEAD is deliberately lenient here: CI runs a tiny grid
# for well under a second under --oversubscribe, where fixed init/finalize
# cost is a large share of the run. The real <1% target is measured on a
# production-sized run with `make overhead-3d`.
SNAPSHOT_MAX_OVERHEAD ?= 0.05

run-3d-snapshot: $(EXAMPLE_3D)
	@echo "Running diffusion3d (SNAPSHOT mode) with $(NRANKS) MPI ranks..."
	MINIPROF_MODE=snapshot \
	MINIPROF_SNAPSHOT=basic \
	MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_3d.json \
	MINIPROF_VERBOSE=1 \
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./$(EXAMPLE_3D)
	@echo ""
	python3 tools/snapshot_check.py miniprof_snapshot_3d.json \
		--nranks $(NRANKS) --max-overhead $(SNAPSHOT_MAX_OVERHEAD)
	@echo ""
	python3 tools/miniprof_report.py miniprof_snapshot_3d.json

# ── run-imbalance-validate: snapshot known-answer test (P1 exit criteria) ───
# examples/imbalance_validate injects a known per-rank compute imbalance and
# a fixed I/O volume; validate_imbalance.py checks the snapshot against the
# analytic answer and against trace mode's Table 2 for the same workload.
# Needs one core per rank — busy-polling ranks on an oversubscribed core
# steal time from the spinning ones and genuinely change the answer — so
# the rank count is capped at the core count.
IV_NRANKS ?= 2
IV_ARGS   ?= 40 10 10
IV_RANKS   = $(shell n=$$(nproc); [ $$n -lt $(IV_NRANKS) ] && echo $$n || echo $(IV_NRANKS))

examples/imbalance_validate: examples/imbalance_validate.c $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) imbalance_validate

run-imbalance-validate: examples/imbalance_validate
	@echo "Running imbalance_validate (SNAPSHOT mode) with $(IV_RANKS) MPI ranks..."
	MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_iv.json \
	mpirun $(MPIRUN_FLAGS) -n $(IV_RANKS) ./examples/imbalance_validate $(IV_ARGS)
	@echo "Running imbalance_validate (TRACE mode) for the cross-check..."
	MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=/dev/null \
	MINIPROF_TRACE_OUTPUT=miniprof_trace_report_iv.txt \
	mpirun $(MPIRUN_FLAGS) -n $(IV_RANKS) ./examples/imbalance_validate $(IV_ARGS)
	@echo ""
	python3 tools/miniprof_report.py miniprof_snapshot_iv.json
	set -- $(IV_ARGS); python3 tools/validate_imbalance.py miniprof_snapshot_iv.json \
		--steps $$1 --base-ms $$2 --delta-ms $$3 \
		--trace-report miniprof_trace_report_iv.txt

# ── run-omp-validate: snapshot OpenMP via OMPT (P2) ─────────────────────────
# examples/omp_validate injects serial time, balanced/imbalanced regions, a
# contended critical section and an imbalanced static loop; the validator
# checks them within a few percent. ranks x threads is capped at nproc:
# oversubscribed threads turn spin time into scheduler noise. OMPT needs
# an LLVM-family runtime — build with e.g. OMPI_CC=clang (or AOCC); under
# GCC libgomp the validator exits 77 and this reports SKIP.
OV_NRANKS  ?= 2
OV_THREADS ?= 4
OV_CPUS     = $(shell nproc)
OV_RANKS    = $(shell r=$(OV_NRANKS); [ $$((r*2)) -gt $(OV_CPUS) ] && r=1; echo $$r)
OV_T        = $(shell t=$(OV_THREADS); m=$$(( $(OV_CPUS) / $(OV_RANKS) )); \
                [ $$t -gt $$m ] && t=$$m; [ $$t -lt 1 ] && t=1; echo $$t)

examples/omp_validate: examples/omp_validate.c $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) omp_validate

run-omp-validate: examples/omp_validate
	@echo "--- omp_validate: $(OV_RANKS) ranks x $(OV_T) threads"
	@OMP_NUM_THREADS=$(OV_T) MINIPROF_MODE=snapshot \
	MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_omp.json \
	mpirun $(MPIRUN_FLAGS) -n $(OV_RANKS) -x OMP_NUM_THREADS --bind-to none \
		./examples/omp_validate || exit 1; \
	python3 tools/validate_omp.py miniprof_snapshot_omp.json --threads $(OV_T); rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: OpenMP runtime without OMPT (build with OMPI_CC=clang/AOCC)"; \
	elif [ $$rc -ne 0 ]; then exit 1; fi

# ── test-ompt-abi: src/ompt_abi.h vs the real <omp-tools.h> ─────────────────
# Compile-only static asserts. Uses OMPT_ABI_CC (default: clang, else cc);
# SKIP when no compiler finds omp-tools.h.
OMPT_ABI_CC ?= $(shell command -v clang 2>/dev/null || echo cc)
test-ompt-abi:
	@if echo '#include <omp-tools.h>' | $(OMPT_ABI_CC) -fopenmp -E -x c - >/dev/null 2>&1; then \
		$(OMPT_ABI_CC) -fopenmp -fsyntax-only -Wall tests/test_ompt_abi.c && \
		echo "test-ompt-abi: OK ($(OMPT_ABI_CC))"; \
	else echo "SKIP: no omp-tools.h for $(OMPT_ABI_CC)"; fi

# ── run-counters-validate: snapshot hardware counters (P3) ──────────────────
# Runs examples/counters_validate in the base group (MINIPROF_SNAPSHOT=basic)
# and the vector group (=roofline) and checks exact region FLOPs, thread
# coverage and the compute/MPI split. Exit 77 from the validator = counters
# unavailable (VM, paranoid) → reported as SKIP, not a failure. The cross-
# check against `perf stat` lives in tools/validate_counters.sh.
CV_ARGS ?= 2097152 20

examples/counters_validate: examples/counters_validate.c $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) counters_validate

run-counters-validate: examples/counters_validate
	@FAIL=0; \
	echo "--- counter engine plumbing (software events, MINIPROF_COUNTERS_TEST=1)"; \
	MINIPROF_MODE=snapshot MINIPROF_COUNTERS_TEST=1 \
	MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_cv_swtest.json \
	mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./examples/counters_validate $(CV_ARGS) || FAIL=1; \
	set -- $(CV_ARGS); python3 tools/validate_counters.py \
		miniprof_snapshot_cv_swtest.json --n $$1 --reps $$2; rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP: software perf events unavailable"; \
	elif [ $$rc -ne 0 ]; then FAIL=1; fi; \
	for lvl in basic roofline; do \
		echo "--- MINIPROF_SNAPSHOT=$$lvl"; \
		MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=$$lvl \
		MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_cv_$$lvl.json \
		mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./examples/counters_validate $(CV_ARGS) || FAIL=1; \
		set -- $(CV_ARGS); python3 tools/validate_counters.py \
			miniprof_snapshot_cv_$$lvl.json --n $$1 --reps $$2; rc=$$?; \
		if [ $$rc -eq 77 ]; then echo "SKIP: hardware counters unavailable on this system"; \
		elif [ $$rc -ne 0 ]; then FAIL=1; fi; \
	done; exit $$FAIL

# ── run-fortran-sentinels: Fortran sentinel regression, every mode ──────────
examples/fortran_sentinels: examples/fortran_sentinels.f90 $(LIB)
	$(MAKE) -C $(EXAMPLE_DIR) fortran_sentinels

run-fortran-sentinels: examples/fortran_sentinels
	@FAIL=0; for m in off sample trace snapshot; do \
		echo "--- MINIPROF_MODE=$$m"; \
		MINIPROF_MODE=$$m MINIPROF_TRACE_FUNCS=/dev/null \
		MINIPROF_OUTPUT=miniprof_report_fs.txt MINIPROF_MPA_OUTPUT=miniprof_fs.mpa \
		MINIPROF_TRACE_OUTPUT=miniprof_trace_report_fs.txt \
		MINIPROF_SNAPSHOT_OUTPUT=miniprof_snapshot_fs.json \
		mpirun $(MPIRUN_FLAGS) -n $(NRANKS) ./examples/fortran_sentinels \
			&& echo "PASS: results correct under MINIPROF_MODE=$$m" \
			|| { echo "FAIL: wrong results under MINIPROF_MODE=$$m"; FAIL=1; }; \
	done; \
	python3 -c "import json,sys; \
	c={f['name']:f['calls'] for f in json.load(open('miniprof_snapshot_fs.json'))['metrics']['parallel']['mpi']['by_function']}; \
	ok=c.get('MPI_Allreduce')==2*$(NRANKS) and c.get('MPI_Isend')==2*$(NRANKS) and c.get('MPI_Bcast')==$(NRANKS); \
	print(('PASS' if ok else 'FAIL')+': Fortran call counts not doubled (Allreduce %s, Isend %s, Bcast %s)' % (c.get('MPI_Allreduce'), c.get('MPI_Isend'), c.get('MPI_Bcast'))); \
	sys.exit(0 if ok else 1)" || FAIL=1; \
	exit $$FAIL

# ── overhead-3d: snapshot vs off, wall clock (informational, not in check) ──
# Uses whatever diffusion3d was last built — build it at a realistic size
# first, e.g.  make example-3d NX=256 NY=256 NZ=128 NSTEPS_3D=400
OVERHEAD_REPS ?= 5
OVERHEAD_MODE ?= snapshot

overhead-3d: $(EXAMPLE_3D)
	./tools/overhead.sh $(NRANKS) $(OVERHEAD_REPS) $(OVERHEAD_MODE) \
		./$(EXAMPLE_3D) $(MPIRUN_FLAGS)

# ── check: full test suite, one pass/fail exit code ──────────────────────────
# What this actually tests, in two tiers:
#   - The three hwc_validate targets assert on report *content* (correct
#     classification, or the smearing warning firing/staying silent) —
#     they SKIP (not FAIL) if hardware counters aren't available on this
#     system, which is expected and common in CI containers.
#   - run/run-trace/run-f/run-f-trace/run-3d/run-3d-trace don't assert on
#     content, but a nonzero exit here is still a real regression signal:
#     it means the build is broken, a PMPI symbol failed to resolve, or
#     something segfaulted — exactly the class of bug this suite exists
#     to catch on every push. See CI: .github/workflows/ci.yml.
#
# Override MPIRUN_FLAGS/NRANKS for constrained environments, e.g.:
#   make check MPIRUN_FLAGS=--oversubscribe NRANKS=2
check: lib examples mpa-check
	@echo "=========================================="
	@echo " miniprof test suite"
	@echo "=========================================="
	@FAIL=0; \
	$(MAKE) test-units             || FAIL=1; \
	$(MAKE) run-hwc-validate       || FAIL=1; \
	$(MAKE) run-hwc-validate-omp   || FAIL=1; \
	$(MAKE) run-hwc-validate-smear || FAIL=1; \
	$(MAKE) run          || FAIL=1; \
	$(MAKE) run-trace    || FAIL=1; \
	$(MAKE) run-f        || FAIL=1; \
	$(MAKE) run-f-trace  || FAIL=1; \
	$(MAKE) run-3d       || FAIL=1; \
	$(MAKE) run-3d-trace || FAIL=1; \
	$(MAKE) run-ext-trace || FAIL=1; \
	$(MAKE) run-3d-snapshot || FAIL=1; \
	$(MAKE) run-imbalance-validate || FAIL=1; \
	$(MAKE) run-fortran-sentinels || FAIL=1; \
	$(MAKE) run-counters-validate || FAIL=1; \
	$(MAKE) test-ompt-abi || FAIL=1; \
	$(MAKE) run-omp-validate || FAIL=1; \
	echo ""; \
	echo "=========================================="; \
	if [ $$FAIL -eq 0 ]; then \
		echo " ALL CHECKS PASSED"; \
	else \
		echo " SOME CHECKS FAILED — see output above"; \
	fi; \
	echo "=========================================="; \
	exit $$FAIL

# ── clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f $(LIB_OBJS) $(LIB) $(EXAMPLE) $(EXAMPLE_TRACE) \
	      $(EXAMPLE_F) $(EXAMPLE_F_TRACE) \
	      $(EXAMPLE_3D) $(EXAMPLE_3D_TRACE) \
	      $(EXAMPLE_HWC_VALIDATE) $(EXAMPLE_HWC_VALIDATE_OMP) \
	      $(EXAMPLE_HWC_VALIDATE_SMEAR) \
	      $(EXAMPLE_MPI_EXT_VALIDATE) $(EXAMPLE_MPI_EXT_VALIDATE_F) mpa-check \
	      miniprof_report.txt miniprof.mpa miniprof_trace_report.txt \
	      miniprof_report_f.txt miniprof_f.mpa miniprof_trace_report_f.txt \
	      miniprof_report_3d.txt miniprof_3d.mpa miniprof_trace_report_3d.txt \
	      miniprof_hwc_validate_report.txt miniprof_hwc_validate.mpa \
	      miniprof_hwc_validate_omp_report.txt miniprof_hwc_validate_omp.mpa \
	      miniprof_hwc_validate_smear_report.txt miniprof_hwc_validate_smear.mpa \
	      miniprof_ext_trace_c.txt miniprof_ext_trace_f.txt \
	      miniprof_snapshot.json miniprof_snapshot_3d.json \
	      miniprof_snapshot_iv.json miniprof_trace_report_iv.txt \
	      miniprof_snapshot_fs.json miniprof_report_fs.txt miniprof_fs.mpa \
	      miniprof_trace_report_fs.txt \
	      examples/imbalance_validate examples/fortran_sentinels \
	      examples/counters_validate miniprof_snapshot_cv_*.json \
	      examples/omp_validate examples/omp_validate_clang examples/omp_validate_gcc \
	      tests/test_units tests/test_units_out.json
