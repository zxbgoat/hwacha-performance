# RTL / Spike 校准基准
HWROOT  ?= $(HOME)/hwacha-compiler
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
SPIKE   := $(HWROOT)/install/bin/spike --isa=rv64gc --extension=hwacha
# LANES=1/2/4 选择仿真器（HwachaRocketConfig / HwachaL2RocketConfig / HwachaL4RocketConfig），也可直接给 SIM=
LANES   ?= 1
SIMNAME := $(if $(filter 1,$(LANES)),HwachaRocketConfig,HwachaL$(LANES)RocketConfig)
SIM     ?= $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-$(SIMNAME)
LOGSUF  := $(if $(filter 1,$(LANES)),,-l$(LANES))
N       ?= 4096

all: bench-n$(N).riscv
bench-n$(N).riscv: main.c kernels.S ../kernels/*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ main.c kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
spike: bench-n$(N).riscv
	$(SPIKE) $<
LOGTAG ?=
rtl: bench-n$(N).riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/rtl-n$(N)$(LOGSUF)$(LOGTAG).log
clean:
	rm -f *.riscv

micro-n$(N).riscv: micro.c micro_kernels.S ../kernels/micro_*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -o $@ micro.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
micro-spike: micro-n$(N).riscv
	$(SPIKE) $<
micro-rtl: micro-n$(N).riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/micro-n$(N)$(LOGSUF)$(LOGTAG).log

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
MAXERR ?= 12
CFG    := $(if $(filter 1,$(LANES)),../configs/rtl-hwacha-rocket.json,../configs/rtl-hwacha-rocket-l$(LANES).json)
calibrate: bench-n$(N).riscv micro-n$(N).riscv
	$(MAKE) spike N=$(N) | grep -a "VERIF"
	$(MAKE) rtl N=$(N) LANES=$(LANES) > /dev/null
	$(MAKE) micro-rtl N=$(N) LANES=$(LANES) > /dev/null
	python3 ../scripts/compare_rtl.py --logs results/rtl-n$(N)$(LOGSUF)$(LOGTAG).log,results/micro-n$(N)$(LOGSUF)$(LOGTAG).log --config $(CFG) --max-err $(MAXERR)
# TileLink 通道级跟踪（需要 +verbose，慢 30–50 倍；见 docs/24 10.1 节）
tl-trace: micro-n$(N).riscv
	mkdir -p results && $(SIM) +permissive +verbose +hwacha_tl_trace=1 +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -E "^(HTL|L2 |RESULT|N=)" > results/tlv-micro-n$(N)$(LOGSUF)$(LOGTAG).log
	python3 ../scripts/tl_trace_stats.py results/tlv-micro-n$(N)$(LOGSUF)$(LOGTAG).log

probe4.riscv: probe4.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe4.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe4-rtl: probe4.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe4.log

probe5.riscv: probe5.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ probe5.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
probe5-rtl: probe5.riscv
	mkdir -p results && $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -av "^\[UART\]" | tee results/probe5.log

# 跳过 gather 的基准（多 lane 索引访存在 RTL 上可能挂起/断言）
bench-nogather-n$(N).riscv: main.c kernels.S ../kernels/*.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -DN=$(N) -DNO_GATHER -o $@ main.c kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
rtl-nogather: bench-nogather-n$(N).riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/rtl-nogather-n$(N)$(LOGSUF)$(LOGTAG).log

probe6.riscv: probe6.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
probe6-rtl: probe6.riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/probe6.log

probe7.riscv: probe7.c micro_kernels.S $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
probe7-rtl: probe7.riscv
	mkdir -p results && stdbuf -oL $(SIM) +permissive +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee results/probe7$(LOGSUF)$(LOGTAG).log
probe7-tl-trace: probe7.riscv
	mkdir -p results && $(SIM) +permissive +verbose +hwacha_tl_trace=1 +max-cycles=4000000000 +loadmem=$< +permissive-off $< 2>&1 | grep -a --line-buffered -E "^(HTL|L2 |RESULT|N=|DONE)" > results/tlv-probe7$(LOGSUF)$(LOGTAG).log
