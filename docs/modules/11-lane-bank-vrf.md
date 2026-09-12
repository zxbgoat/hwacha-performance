# 11 向量 lane、bank 与寄存器堆

源码：`lane.scala`（`Lane`）、`lane-ctrl.scala`（`LaneCtrl`）、`bank.scala`（`Bank`）、`bank-rf.scala`（`BankRegfile`）、`util-confprec.scala`（`PackLogic`、`RateLogic`）

## 1. lane 的组成

```
                     ┌──────── Lane ────────────────────────────────────────────┐
 展开器 µop ──▶ LaneCtrl ──▶ Bank0 ─▶ Bank1 ─▶ Bank2 ─▶ Bank3   （systolic 传递）  │
                             │ VRF   │ VRF   │ VRF   │ VRF                      │
                             │ PRF   │ PRF   │ PRF   │ PRF                      │
                             │ ALU×2 │ ALU×2 │ ALU×2 │ ALU×2  （每 slice 一个）  │
                             │ PLU   │ PLU   │ PLU   │ PLU                      │
                             │ OPL/PDL 锁存器                                    │
                             │ BRQ/BPQ/BWQ                                       │
                             └───┬───┴───┬───┴───┬───┴───┬──                     │
                                 └───────┴───────┴───────┘                       │
                                   操作数交叉开关 / 谓词交叉开关                   │
                                     │            │                              │
                              VFU0: FMA0 IMul FConv   VFU1: FMA1 FCmp            │
                                                                                 │
                      LPQ/LRQ ──▶ DCC（VGU/VPU/VSU/VLU/VDU）──▶ VMU              │
                     └───────────────────────────────────────────────────────────┘
```

- 4 个 bank，每个 128 位宽，分成 2 个 64 位 slice。
- `LaneCtrl` 把展开器送来的 µop 转成每个 bank 的控制信号并逐拍传递。
- 操作数交叉开关把各 bank 的全局操作数锁存器（`gopl`）汇聚（按位 OR，因为同一时刻只有一个 bank 驱动）送到共享功能单元；谓词交叉开关同理（`gpdl`）。
- 标量操作数经 `splat_scalar` 复制到每个 slice 后与锁存器输出复用。
- 共享功能单元按 slice 例化：每个 lane 有 2 个 IMul slice、2×2 个 FMA slice（nVFMU = 2）、2 个 FCmp slice、2 个 FConv slice，即每周期 2 个 64 位元素通过每个单元。

## 2. bank 内部

**向量寄存器堆 VRF**：`sram_rf = SeqMem(256, 16 字节)`，即 256 行 × 128 位的单端口同步 SRAM（物理上是 1R1W 8T SRAM 宏）。一行存 2 个 64 位、4 个 32 位或 8 个 16 位元素。读出数据经 `unpack_bank` 按精度解包。写端口由 3 路仲裁器 `sram_warb` 共享：展开器排定的 sram-write、来自访存的 BWQ（`bwqs.mem`）、来自变延迟单元的 BWQ（`bwqs.fu`）。仲裁器 out.ready 恒真，写永远可以进行。

**触发器寄存器堆 FF RF**：`ff_rf = Mem(16, 16 字节)`，16 条目、3 读口的小型触发器阵列，源码中 `selff` 选择位目前固定为 false（FIXME），即当前未启用。

**谓词寄存器堆 PRF**：`pred_rf = Mem(256, wPred 位)`，256 行；论文微架构手册写 3R1W，博士论文写 5R1W，开源代码参数 `HwachaNPredRFReadPorts = 3`。混合精度模式下 wPred 加宽到 8 位，让一行 SRAM 中 8 个 halfword 的谓词能一次读出。有一个 gated read 端口（`gread`，读出后用于门控后续 SRAM 读，`unpred` 配置下可跳过）和普通读端口。

**操作数锁存器**：`gopl`（全局，nGOPL = 6）供共享功能单元；`lopl`（本地）供本 bank 的 ALU。谓词锁存器 `gpdl`（4）/`lpdl` 同理。

**本地功能单元**：每个 slice 一个 `ALUSlice`（整数加减、移位、逻辑、比较、veidx），一个 `PLUSlice`（三输入真值表逻辑）。它们直接挂在 bank 读写口上，不经过交叉开关，因此整数操作没有结构冒险，操作数搬运能耗最低。

**队列**：
- BRQ（bank read queue，深度 4）：VSU/AMO 数据读出。
- BPQ（bank predicate queue，深度 2×nBanks）：VPU 谓词读出给 VMU。
- BWQ（bank write queue，深度 2）：VLU（mem）与 VDU（fu）的解耦写回。

## 3. 架构寄存器到物理行的映射

RoCC 单元在 `vsetcfg` 时把 SRAM 划分为 doubleword、word、halfword 三个区域，并给出各区域的物理基址与步长（`cfg.base`、`cfg.vstride`）：

```
物理行 = base[prec] + 架构寄存器号 + strip索引 × vstride[prec]
```

也就是说，同一架构寄存器的连续 strip 相隔 `vstride`（该精度的寄存器总数）行，而同一 strip 的不同寄存器相邻。这样：
- 序列器只需按常数步进物理行号；
- 同一元素索引的所有寄存器永远落在同一 bank，避免了不同 bank 需要不同 µop 调度的结构冒险；
- 谓词寄存器按 `pstride = nvp` 同样条带化。

## 4. 元素在 lane 内的布局

一个 strip 的 8 个 64 位元素分布为：bank0 slice0/1 = 元素 0/1，bank1 = 元素 2/3，bank2 = 4/5，bank3 = 6/7。因此 µop 逐 bank 传递时自然按元素顺序推进。混合精度下一行放更多元素，`RateLogic`/`PackLogic` 负责计算每拍处理的子字数与解包/重打包 mux。

## 5. 与 45nm 芯片的差异

EOS 芯片（ESSCIRC 2014）用 8 个 64 位 bank；第四代改为 4 个 128 位 bank，配合两个 FMA 簇，功能单元吞吐翻倍，而 SRAM 总容量不变（4 × 256 × 128 位 = 16 KB，每 lane）。
