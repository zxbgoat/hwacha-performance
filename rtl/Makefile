# RTL / Spike 校准基准
HWROOT  := $(HOME)/hwacha-compiler
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
SPIKE   := $(HWROOT)/install/bin/spike --isa=rv64gc --extension=hwacha
SIM     ?= $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-HwachaRocketConfig
SIM_L2  := $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-HwachaL2RocketConfig
N       ?= 4096

all: bench-n$(N).riscv
bench-n$(N).riscv: main.c kernels.S ../kernels/*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ main.c kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
spike: bench-n$(N).riscv
	$(SPIKE) $<
LOGTAG ?=
rtl: bench-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/rtl-n$(N)$(LOGTAG).log
clean:
	rm -f *.riscv

micro-n$(N).riscv: micro.c micro_kernels.S ../kernels/micro_*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ micro.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
micro-spike: micro-n$(N).riscv
	$(SPIKE) $<
micro-rtl: micro-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/micro-n$(N)$(LOGTAG).log

probe.riscv: probe.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe-rtl: probe.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe.log

probe2.riscv: probe2.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe2.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe2-rtl: probe2.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe2.log

probe3.riscv: probe3.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe3.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe3-rtl: probe3.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe3.log

# 一键校准回归：Spike 验证 → RTL 计时（基准 + 微基准）→ 与模型比较（最大误差阈值 MAXERR%）
MAXERR ?= 10
calibrate: bench-n$(N).riscv micro-n$(N).riscv
	$(MAKE) spike N=$(N) | grep -a "VERIF"
	$(MAKE) rtl N=$(N) > /dev/null
	$(MAKE) micro-rtl N=$(N) > /dev/null
	python3 ../scripts/compare_rtl.py --logs results/rtl-n$(N).log,results/micro-n$(N).log --no-py --max-err $(MAXERR)

probe4.riscv: probe4.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe4.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe4-rtl: probe4.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe4.log

probe5.riscv: probe5.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe5.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe5-rtl: probe5.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe5.log

# 2 lane RTL：跳过会触发 IBoxML 断言的 gather
bench-nogather-n$(N).riscv: main.c kernels.S ../kernels/*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -DNO_GATHER -o $@ main.c kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
rtl-nogather: bench-nogather-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/rtl-nogather-n$(N)$(LOGTAG).log

probe6.riscv: probe6.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
probe6-rtl: probe6.riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/probe6.log
