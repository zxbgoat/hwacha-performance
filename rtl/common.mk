# rtl/、rtl/hcc/、rtl/rodinia/ 共用的工具链与仿真器选择
HWROOT  ?= $(HOME)/hwacha-compiler
COMMON  := $(HWROOT)/esp-tests/benchmarks/common
CC      := $(HWROOT)/chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-gcc
CFLAGS  := -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -fno-common -fno-builtin-printf \
           -fno-tree-loop-distribute-patterns -march=rv64gcxhwacha -mabi=lp64d -I$(COMMON) -I$(HWROOT)/esp-tests/env
LDFLAGS := -static -nostdlib -nostartfiles -lm -lgcc -T $(COMMON)/test.ld
SPIKE   := $(HWROOT)/install/bin/spike --isa=rv64gc --extension=hwacha
SPIKE_HLOG := $(HWROOT)/install-hlog/bin/spike --isa=rv64gc --extension=hwacha
# LANES=1/2/4/8/16 选仿真器（HwachaRocketConfig / HwachaL<N>RocketConfig）；BANKS=2/4 选多 bank 变体（HwachaL<N>B<B>RocketConfig）；也可直接给 SIM=
LANES   ?= 1
BANKS   ?= 1
SIMNAME := $(if $(filter 1,$(LANES)),HwachaRocketConfig,HwachaL$(LANES)$(if $(filter 1,$(BANKS)),,B$(BANKS))RocketConfig)
SIM     ?= $(HWROOT)/chipyard/sims/verilator/simulator-chipyard.harness-$(SIMNAME)
LOGSUF  := $(if $(filter 1,$(LANES)),,-l$(LANES))$(if $(filter 1,$(BANKS)),,-b$(BANKS))
LOGTAG  ?=
SIMARGS := +permissive +max-cycles=4000000000
# RUN_RTL,<binary>,<logfile>：行缓冲地跑仿真并把去掉 UART 噪音的输出写进日志
define RUN_RTL
mkdir -p $(dir $(2)) && stdbuf -oL $(SIM) $(SIMARGS) +loadmem=$(1) +permissive-off $(1) 2>&1 | grep -a --line-buffered -v "^\[UART\]" | tee $(2)
endef
# RUN_TL_TRACE：带 +verbose +hwacha_tl_trace 的运行，只保留通道跟踪与结果行
define RUN_TL_TRACE
mkdir -p $(dir $(2)) && $(SIM) +permissive +verbose +hwacha_tl_trace=1 +max-cycles=4000000000 +loadmem=$(1) +permissive-off $(1) 2>&1 | grep -a --line-buffered -E "^(HTL|L2 |RESULT|N=|DONE|PROBE)" > $(2)
endef
