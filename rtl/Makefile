# RTL / Spike 校准基准
HWROOT  := $(HOME)/hwacha-compiler
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
SPIKE   := $(HWROOT)/install/bin/spike --isa=rv64gc --extension=hwacha
SIM     := $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-HwachaRocketConfig
N       ?= 4096

all: bench-n$(N).riscv
bench-n$(N).riscv: main.c kernels.S ../kernels/*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ main.c kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
spike: bench-n$(N).riscv
	$(SPIKE) $<
rtl: bench-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/rtl-n$(N).log
clean:
	rm -f *.riscv

micro-n$(N).riscv: micro.c micro_kernels.S ../kernels/micro_*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ micro.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
micro-spike: micro-n$(N).riscv
	$(SPIKE) $<
micro-rtl: micro-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/micro-n$(N).log

probe.riscv: probe.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe-rtl: probe.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe.log

probe2.riscv: probe2.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe2.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe2-rtl: probe2.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe2.log
