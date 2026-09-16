# 值得向上游报告的两件事（未提交，正文供参考）

## 1. hwacha：`IBoxML` 的 `qcntr` 在 2 lane 以上做索引访存时溢出（`vmu.scala`）

**Title**: IBoxML: `qcntr` overflows on indexed memory ops with nLanes > 1 (assert "IBox: qcntr too large. aret broken")

**Body**:

With `WithNLanes(2)` (or 4/8/16) on Chipyard 1.11 (`hwacha` @ bf799dc), any indexed vector load/store (`vlxw`, `vsxw`, …) whose vector length spans more than a couple of strips fails the assertion in `IBoxML`:

```
IBox: qcntr too large. aret broken
```

`IBoxML` splits one VMU op into per-lane-strip sub-ops and tracks how many of them are still queued in the `abox2` issue queue with a 2-bit counter `qcntr = qcnts(3) + 1 + (issue(3).fire ? 0 : 1)`, then fires `aret` when it counts down to 1. For unit-stride ops the AGU issues at most one sub-op every few cycles so the queue never holds more than one entry; for indexed ops the sub-ops are enqueued every cycle, the queue (`nVMUIQ = 2`) fills, `qcntr` wraps and the `aret` accounting breaks. Symptoms after the assert is disabled: the MRT address queue is never popped and the sequencer deadlocks on the next memory op.

Fix that works for us (2/4/8/16 lanes, verified against Spike and with the RTL cycle counts of a gather kernel matching a calibrated performance model within 3%): tag every sub-op with `last := vlen_end` in `VMUDecodedOp`, and drive `io.aret := io.issue(3).fire && io.issue(3).bits.last` — i.e. return the address entry exactly when abox2 dequeues the last sub-op of the op. `qcntr`/`aret_pending` and the assertion go away. Diff: `chipyard-hwacha-generator.patch` in this repo (the `vmu.scala` hunk).

Reproducer: `rtl/main.c` + `kernels/gather.S` from https://github.com/<user>/hwacha-performance, built against `HwachaL2RocketConfig = WithNLanes(2) ++ HwachaRocketConfig`.

## 2. chipyard / hwacha：`HwachaBuildVRU` 默认为 true，但默认 harness 的 `SimDRAM` 没有延迟，VRU 无法评估

**Title**: Hwacha VRU is enabled by default but unobservable with the default SimDRAM harness

**Body**:

`hwacha.DefaultHwachaConfig` sets `HwachaBuildVRU => true`. In the default Chipyard Verilator harness the memory model (`SimDRAM` / `mm_magic`) has near-zero latency and unbounded bandwidth: a 32 KB vector load whose lines were just evicted from L2 takes 2103 cycles vs 2137 when they hit (probe4 in the repo above), and a single strip from "DRAM" is 44–69 cycles vs 35 on a hit. Rebuilding with `HwachaBuildVRU => false` produces byte-identical cycle counts on every probe we have. So anyone trying to evaluate the runahead unit on the stock config will see no effect and may conclude it does nothing. Suggest either documenting this next to `HwachaBuildVRU`, or pointing to a FASED/DRAMSim-backed harness config as the one to use for VRU experiments.
