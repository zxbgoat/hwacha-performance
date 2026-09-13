# Rodinia（hwacha-cc 编译）内核的 RTL 三次计时：冷 / warm / warm2
HWROOT  := $(HOME)/hwacha-compiler
APPS    := $(HWROOT)/hwacha-cc/test/apps
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -fno-math-errno -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env -I.
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
SIM     ?= $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-HwachaRocketConfig
LOGTAG  ?=
PROGS   := kmeans nn pgain pathfinder
all: $(addsuffix .riscv,$(PROGS))
%.riscv: %_main.c $(APPS)/%.s $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
%.spike: %.riscv
	$(HWROOT)/install-hlog/bin/spike --isa=rv64gc --extension=hwacha $< 2>/dev/null
%.rtl: %.riscv
	mkdir -p ../results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee ../results/rodinia-$*$(LOGTAG).log
rtl: $(addsuffix .rtl,$(PROGS))
clean:
	rm -f *.riscv
