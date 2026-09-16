include ../common.mk
APPS    := $(HWROOT)/hwacha-cc/test/apps
CFLAGS += -fno-math-errno -I.
PROGS   := kmeans nn pgain pathfinder
all: $(addsuffix .riscv,$(PROGS))
%.riscv: %_main.c $(APPS)/%.s $(COMMON)/crt.S $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
%.spike: %.riscv
	$(HWROOT)/install-hlog/bin/spike --isa=rv64gc --extension=hwacha $< 2>/dev/null
%.rtl: %.riscv
	$(call RUN_RTL,$<,../results/rodinia-$*$(LOGSUF)$(LOGTAG).log)
rtl: $(addsuffix .rtl,$(PROGS))
clean:
	rm -f *.riscv
