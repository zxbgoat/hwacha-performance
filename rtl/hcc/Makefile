HWROOT  ?= $(HOME)/hwacha-compiler
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -DHAVE_HAND -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env -I$(COMMON)/../vec-daxpy
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
LANES   ?= 1
LOGTAG  ?=
SIMNAME := $(if $(filter 1,$(LANES)),HwachaRocketConfig,HwachaL$(LANES)RocketConfig)
SIM     ?= $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-$(SIMNAME)
LOGSUF  := $(if $(filter 1,$(LANES)),,-l$(LANES))
N ?= 1024
hcc-n$(N).riscv: bench_main.c bench.s hand.S
	$(CC) $(CFLAGS) -DN=$(N) -o $@ bench_main.c bench.s hand.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
rtl: hcc-n$(N).riscv
	mkdir -p ../results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee ../results/hcc-n$(N)$(LOGSUF)$(LOGTAG).log
