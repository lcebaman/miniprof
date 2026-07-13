# npb/Makefile — NAS Parallel Benchmarks with miniprof integration
#
# Prerequisites: mpifort on $PATH, miniprof built (make lib in parent dir)
#
# Variables:
#   CLASS=S        — problem class (S, W, A, B, C, D, E, F)
#   NRANKS=4       — number of MPI ranks
#
# Example:
#   make setup
#   make run-cg CLASS=A NRANKS=8
#   make trace-cg CLASS=A NRANKS=8

# ── configuration ──────────────────────────────────────────────────────────

NPB_URL     := https://www.nas.nasa.gov/assets/npb/NPB3.4.4.tar.gz
NPB_TAR     := NPB3.4.4.tar.gz
NPB_ROOT    := NPB3.4.4
NPB_MPI_DIR := $(NPB_ROOT)/NPB3.4-MPI

CLASS       ?= S
NRANKS      ?= 4

MINIPROF_DIR := ..
MINIPROF_LIB := $(MINIPROF_DIR)/libminiprof.a

FC ?= mpifort
CC ?= mpicc

NPB_CFLAGS     := -O2 -g
NPB_FFLAGS     := -O2 -g
NPB_TRACE_FLAGS := -finstrument-functions
NPB_MINIPROF_LIBS := -L$(CURDIR)/$(MINIPROF_DIR) -lminiprof -ldl -lm

BENCHMARKS  := IS EP CG MG FT BT SP LU

.PHONY: download setup clean mpa-check \
        build-all build-all-trace run-all trace-all \
        $(addprefix build-,$(BENCHMARKS)) \
        $(addprefix build-,$(BENCHMARKS:%=%-trace)) \
        $(addprefix run-,$(BENCHMARKS)) \
        $(addprefix trace-,$(BENCHMARKS))

# ── download NPB ───────────────────────────────────────────────────────────

$(NPB_ROOT):
	@echo "=== Downloading NPB 3.4.4 ==="
	wget -q $(NPB_URL) -O $(NPB_TAR)
	tar xzf $(NPB_TAR)
	rm -f $(NPB_TAR)
	@echo "=== Extracted to $(NPB_ROOT) ==="

download: $(NPB_ROOT)

# ── setup: generate make.def ───────────────────────────────────────────────

setup: $(NPB_ROOT) $(MINIPROF_LIB)
	@echo "=== Generating $(NPB_MPI_DIR)/config/make.def ==="
	@mkdir -p $(NPB_MPI_DIR)/config
	@printf '%s\n' \
	  "SUF        = .f90" \
	  "MPIFC      = $(FC)" \
	  "FLINK      = $(FC)" \
	  "FFLAGS     = $(NPB_FFLAGS)" \
	  "FLINKFLAGS = $(NPB_FFLAGS)" \
	  "FMPI_INC   =" \
	  "FMPI_LIB   = $(NPB_MINIPROF_LIBS)" \
	  "CC         = $(CC)" \
	  "CLINK      = $(CC)" \
	  "C_LIB      = -lm" \
	  "C_INC      =" \
	  "CFLAGS     = $(NPB_CFLAGS)" \
	  "CLINKFLAGS = $(NPB_CFLAGS)" \
	  "UCC        = gcc" \
	  "BINDIR     = ../bin" \
	  "RAND       = randi8" \
	  "WTIME      = wtime.c" \
	  > $(NPB_MPI_DIR)/config/make.def
	@mkdir -p $(NPB_MPI_DIR)/bin
	@echo "  Generated make.def"

# ── mpa-check validator ───────────────────────────────────────────────────

mpa-check: $(MINIPROF_DIR)/src/mpa_check.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

# ════════════════════════════════════════════════════════════════════════════
#  IS — Integer Sort
# ════════════════════════════════════════════════════════════════════════════

build-is: setup
	@echo "=== IS.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/IS && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/IS.$(CLASS).$(NRANKS).x bin/

build-is-trace: setup
	@echo "=== IS.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/IS && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/IS.$(CLASS).$(NRANKS).x bin/IS.$(CLASS).$(NRANKS).x+trace

run-is: build-is
	@echo "=== Running IS (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_IS.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_IS.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/IS.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (IS) ===" && cat miniprof_report_IS.txt
	@echo "" && echo "=== miniprof_IS.mpa ===" && cat miniprof_IS.mpa

trace-is: build-is build-is-trace mpa-check
	@echo "=== Validating miniprof_IS.mpa ==="
	./mpa-check bin/IS.$(CLASS).$(NRANKS).x+trace miniprof_IS.mpa
	@echo "=== Running IS (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_IS.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_IS.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/IS.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (IS) ===" && cat miniprof_trace_report_IS.txt

# ════════════════════════════════════════════════════════════════════════════
#  EP — Embarrassingly Parallel
# ════════════════════════════════════════════════════════════════════════════

build-ep: setup
	@echo "=== EP.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/EP && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/EP.$(CLASS).$(NRANKS).x bin/

build-ep-trace: setup
	@echo "=== EP.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/EP && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/EP.$(CLASS).$(NRANKS).x bin/EP.$(CLASS).$(NRANKS).x+trace

run-ep: build-ep
	@echo "=== Running EP (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_EP.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_EP.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/EP.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (EP) ===" && cat miniprof_report_EP.txt
	@echo "" && echo "=== miniprof_EP.mpa ===" && cat miniprof_EP.mpa

trace-ep: build-ep build-ep-trace mpa-check
	@echo "=== Validating miniprof_EP.mpa ==="
	./mpa-check bin/EP.$(CLASS).$(NRANKS).x+trace miniprof_EP.mpa
	@echo "=== Running EP (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_EP.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_EP.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/EP.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (EP) ===" && cat miniprof_trace_report_EP.txt

# ════════════════════════════════════════════════════════════════════════════
#  CG — Conjugate Gradient
# ════════════════════════════════════════════════════════════════════════════

build-cg: setup
	@echo "=== CG.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/CG && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/CG.$(CLASS).$(NRANKS).x bin/

build-cg-trace: setup
	@echo "=== CG.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/CG && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/CG.$(CLASS).$(NRANKS).x bin/CG.$(CLASS).$(NRANKS).x+trace

run-cg: build-cg
	@echo "=== Running CG (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_CG.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_CG.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/CG.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (CG) ===" && cat miniprof_report_CG.txt
	@echo "" && echo "=== miniprof_CG.mpa ===" && cat miniprof_CG.mpa

trace-cg: build-cg build-cg-trace mpa-check
	@echo "=== Validating miniprof_CG.mpa ==="
	./mpa-check bin/CG.$(CLASS).$(NRANKS).x+trace miniprof_CG.mpa
	@echo "=== Running CG (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_CG.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_CG.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/CG.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (CG) ===" && cat miniprof_trace_report_CG.txt

# ════════════════════════════════════════════════════════════════════════════
#  MG — Multi-Grid
# ════════════════════════════════════════════════════════════════════════════

build-mg: setup
	@echo "=== MG.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/MG && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/MG.$(CLASS).$(NRANKS).x bin/

build-mg-trace: setup
	@echo "=== MG.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/MG && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/MG.$(CLASS).$(NRANKS).x bin/MG.$(CLASS).$(NRANKS).x+trace

run-mg: build-mg
	@echo "=== Running MG (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_MG.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_MG.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/MG.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (MG) ===" && cat miniprof_report_MG.txt
	@echo "" && echo "=== miniprof_MG.mpa ===" && cat miniprof_MG.mpa

trace-mg: build-mg build-mg-trace mpa-check
	@echo "=== Validating miniprof_MG.mpa ==="
	./mpa-check bin/MG.$(CLASS).$(NRANKS).x+trace miniprof_MG.mpa
	@echo "=== Running MG (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_MG.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_MG.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/MG.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (MG) ===" && cat miniprof_trace_report_MG.txt

# ════════════════════════════════════════════════════════════════════════════
#  FT — Fast Fourier Transform
# ════════════════════════════════════════════════════════════════════════════

build-ft: setup
	@echo "=== FT.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/FT && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/FT.$(CLASS).$(NRANKS).x bin/

build-ft-trace: setup
	@echo "=== FT.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/FT && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/FT.$(CLASS).$(NRANKS).x bin/FT.$(CLASS).$(NRANKS).x+trace

run-ft: build-ft
	@echo "=== Running FT (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_FT.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_FT.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/FT.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (FT) ===" && cat miniprof_report_FT.txt
	@echo "" && echo "=== miniprof_FT.mpa ===" && cat miniprof_FT.mpa

trace-ft: build-ft build-ft-trace mpa-check
	@echo "=== Validating miniprof_FT.mpa ==="
	./mpa-check bin/FT.$(CLASS).$(NRANKS).x+trace miniprof_FT.mpa
	@echo "=== Running FT (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_FT.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_FT.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/FT.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (FT) ===" && cat miniprof_trace_report_FT.txt

# ════════════════════════════════════════════════════════════════════════════
#  BT — Block Tri-diagonal
# ════════════════════════════════════════════════════════════════════════════

build-bt: setup
	@echo "=== BT.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/BT && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/BT.$(CLASS).$(NRANKS).x bin/

build-bt-trace: setup
	@echo "=== BT.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/BT && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/BT.$(CLASS).$(NRANKS).x bin/BT.$(CLASS).$(NRANKS).x+trace

run-bt: build-bt
	@echo "=== Running BT (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_BT.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_BT.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/BT.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (BT) ===" && cat miniprof_report_BT.txt
	@echo "" && echo "=== miniprof_BT.mpa ===" && cat miniprof_BT.mpa

trace-bt: build-bt build-bt-trace mpa-check
	@echo "=== Validating miniprof_BT.mpa ==="
	./mpa-check bin/BT.$(CLASS).$(NRANKS).x+trace miniprof_BT.mpa
	@echo "=== Running BT (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_BT.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_BT.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/BT.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (BT) ===" && cat miniprof_trace_report_BT.txt

# ════════════════════════════════════════════════════════════════════════════
#  SP — Scalar Penta-diagonal
# ════════════════════════════════════════════════════════════════════════════

build-sp: setup
	@echo "=== SP.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/SP && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/SP.$(CLASS).$(NRANKS).x bin/

build-sp-trace: setup
	@echo "=== SP.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/SP && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/SP.$(CLASS).$(NRANKS).x bin/SP.$(CLASS).$(NRANKS).x+trace

run-sp: build-sp
	@echo "=== Running SP (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_SP.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_SP.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/SP.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (SP) ===" && cat miniprof_report_SP.txt
	@echo "" && echo "=== miniprof_SP.mpa ===" && cat miniprof_SP.mpa

trace-sp: build-sp build-sp-trace mpa-check
	@echo "=== Validating miniprof_SP.mpa ==="
	./mpa-check bin/SP.$(CLASS).$(NRANKS).x+trace miniprof_SP.mpa
	@echo "=== Running SP (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_SP.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_SP.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/SP.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (SP) ===" && cat miniprof_trace_report_SP.txt

# ════════════════════════════════════════════════════════════════════════════
#  LU — Lower-Upper Gauss-Seidel
# ════════════════════════════════════════════════════════════════════════════

build-lu: setup
	@echo "=== LU.$(CLASS).$(NRANKS).x (sample) ==="
	cd $(NPB_MPI_DIR)/LU && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS)
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/LU.$(CLASS).$(NRANKS).x bin/

build-lu-trace: setup
	@echo "=== LU.$(CLASS).$(NRANKS).x+trace (trace) ==="
	cd $(NPB_MPI_DIR)/LU && $(MAKE) CLASS=$(CLASS) NP=$(NRANKS) \
	  FFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)" \
	  FLINKFLAGS="$(NPB_FFLAGS) $(NPB_TRACE_FLAGS)"
	mkdir -p bin && cp $(NPB_MPI_DIR)/bin/LU.$(CLASS).$(NRANKS).x bin/LU.$(CLASS).$(NRANKS).x+trace

run-lu: build-lu
	@echo "=== Running LU (SAMPLE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=sample MINIPROF_INTERVAL_MS=5 MINIPROF_DEPTH=10 \
	  MINIPROF_OUTPUT=$(CURDIR)/miniprof_report_LU.txt \
	  MINIPROF_MPA_OUTPUT=$(CURDIR)/miniprof_LU.mpa \
	  MINIPROF_MPA_THRESHOLD=1.0 MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/LU.$(CLASS).$(NRANKS).x
	@echo "" && echo "=== miniprof sampling report (LU) ===" && cat miniprof_report_LU.txt
	@echo "" && echo "=== miniprof_LU.mpa ===" && cat miniprof_LU.mpa

trace-lu: build-lu build-lu-trace mpa-check
	@echo "=== Validating miniprof_LU.mpa ==="
	./mpa-check bin/LU.$(CLASS).$(NRANKS).x+trace miniprof_LU.mpa
	@echo "=== Running LU (TRACE, $(NRANKS) ranks, class $(CLASS)) ==="
	cd $(NPB_MPI_DIR) && \
	  MINIPROF_MODE=trace MINIPROF_TRACE_FUNCS=$(CURDIR)/miniprof_LU.mpa \
	  MINIPROF_TRACE_OUTPUT=$(CURDIR)/miniprof_trace_report_LU.txt \
	  MINIPROF_VERBOSE=1 \
	  mpirun -n $(NRANKS) ./bin/LU.$(CLASS).$(NRANKS).x+trace
	@echo "" && echo "=== miniprof trace report (LU) ===" && cat miniprof_trace_report_LU.txt

# ════════════════════════════════════════════════════════════════════════════
#  Aggregate targets
# ════════════════════════════════════════════════════════════════════════════

build-all:       $(addprefix build-,$(BENCHMARKS))
build-all-trace: $(addprefix build-,$(BENCHMARKS:%=%-trace))
run-all:         $(addprefix run-,$(BENCHMARKS))
trace-all:       $(addprefix trace-,$(BENCHMARKS))

# ── clean ──────────────────────────────────────────────────────────────────

clean:
	rm -rf bin mpa-check
	rm -f miniprof_report_*.txt miniprof_*.mpa miniprof_trace_report_*.txt
	@if [ -d "$(NPB_MPI_DIR)" ]; then cd $(NPB_MPI_DIR) && $(MAKE) clean; fi
