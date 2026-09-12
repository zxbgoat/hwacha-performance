# 08 向量预取单元（VRU）

源码：`vru.scala`（`VRURoCCUnit`、`VRUFrontend`、`RunaheadManager`、`PrefetchUnit`、`VRU`）

## 1. 目的

VRU 是 Hwacha 访存/执行解耦中的"访存处理器"。它不执行任何计算，只做一件事：**比 VXU 更早地看到向量取指块中的单位步长/常量步长访存，并把对应的 cache line 提前拉进 L2**。

它能做到几乎非投机，依赖三个 ISA 约束：

1. 地址寄存器 va 在工作线程中只读，只能由控制线程 `vmca` 写；
2. vlen 只能由控制线程 `vsetvl` 写；
3. 单位步长/常量步长访存的基址与步长只来自 va。

因此 VRU 只需要在 VRCMDQ 上跟踪 `vsetcfg`、`vsetvl`、`vmca`、`vf`，在自己的地址寄存器副本上就能算出每条访存会碰的字节范围，完全不依赖 VXU 的执行结果，没有死锁风险。索引访存（基址在 vs）和 AMO 不会被预取。

## 2. 结构

```
VRCMDQ ──▶ VRURoCCUnit ─(vf: pc, vlen, va 副本)─▶ VRUFrontend ──▶ decodedMemOpQueue(10) ──▶ PrefetchUnit ──▶ L2
                                                     │  ▲ VI$                     ▲
                                                     ▼                            │ 节流
                                              RunaheadManager ◀── 主序列器 vf 完成应答
```

**VRURoCCUnit**：维护 va 寄存器副本和 vlen，译码 VRCMDQ 命令。收到 `vf` 时触发前端。

**VRUFrontend**：从 VI$ 取指（与标量单元共享 VI$，因此也为标量单元预热指令），用独立的小译码表只识别 unit-stride / constant-stride load/store，记录每条的宽度、load/store、当前 vlen 和地址，累加本块的 load/store 字节数，遇到 `vstop` 结束本块并把字节数交给 RunaheadManager。

**RunaheadManager**：两个节流机制。
- *启动跳过*：前 `HwachaVRUEarlyIgnore`（默认 1）个向量块不预取。论文观察到牺牲最初一两个块的预取能显著提高稳态下的领先距离，否则 VRU 一开始就和 VXU 挤在一起，浪费 L2 tracker 并制造 bank 热点。
- *领先距离节流*：`bytesq` 队列为每个已解码但未被 VXU 完成的块记录 load/store 字节数，全局计数器 `runahead_bytes_count` 累加；主序列器每完成一个块（`vf_complete_ack`，来自 `mseq.io.vf.last`）就出队一项并减去对应字节数。全局计数超过 `HwachaVRUMaxRunaheadBytes`（默认 16 MB）时停止译码新块。按块而不是按指令同步，是因为谓词和一致性分支会让 VXU 跳过某些访存，按指令计数会失配。

**PrefetchUnit**：从解码队列取操作，按 cache line 粒度向 L2 发预取请求；限制在途预取数不超过 `HwachaVRUMaxOutstandingPrefetches`（默认 20，注释说明约为 L2 tracker 的三分之一，因为单位步长情况下 VRU 的预取块是执行单元请求的两倍大）。

## 3. 论文中的设计权衡

- VRU 跑得太近：既隐藏不了延迟，又占用 L2 tracker，还会把请求集中到一个 L2 bank。
- VRU 跑得太远：会把 L2 中正在使用或已预取尚未使用的行挤出去。
- 因此需要同时有启动跳过和基于字节数的领先距离控制。

## 4. 开源代码现状

顶层 `hwacha.scala` 中 `confvru` 被硬编码为 `false`，注释为"TODO: Fix prefetcher using TL2 Hints"。`PrefetchUnit` 内也有"fix-up once TL2 supports prefetches (Intent message type)"的注释。也就是说，当前 GitHub 主线**没有例化 VRU**，`HwachaBuildVRU` 参数不起作用，VRCMDQ 直接被 ready 吞掉。Ara 论文指出开源版 Hwacha 无法复现论文中的高 FPU 利用率，L2 与预取器缺失是主要原因之一。若要做预取相关的性能研究，需要自行把 `PrefetchUnit` 迁移到 TileLink2 的 Hint/Intent 消息并打开 `confvru`。

## 5. 相关参数

| 参数 | 默认 | 含义 |
|---|---|---|
| HwachaBuildVRU | true（但被顶层覆盖为 false） | 是否例化 |
| HwachaVRUMaxOutstandingPrefetches | 20 | 在途预取上限 |
| HwachaVRUEarlyIgnore | 1 | 启动时跳过的块数 |
| HwachaVRUMaxRunaheadBytes | 16777216 | 领先字节数上限 |
| HwachaNPTLB | 4 | 预取 TLB 条目（源码中被 SMU 复用） |
