include ../common.mk
CFLAGS += -DHAVE_HAND -I$(COMMON)/../vec-daxpy
N ?= 1024
hcc-n$(N).riscv: bench_main.c bench.s hand.S
	$(CC) $(CFLAGS) -DN=$(N) -o $@ bench_main.c bench.s hand.S $(COMMON)/crt.S $(COMMON)/syscalls.c $(LDFLAGS)
rtl: hcc-n$(N).riscv
	$(call RUN_RTL,$<,../results/hcc-n$(N)$(LOGSUF)$(LOGTAG).log)
