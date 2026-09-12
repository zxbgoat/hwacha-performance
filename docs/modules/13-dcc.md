# 13 解耦簇（DCC）：VGU、VPU、VSU、VLU、VDU

源码：`dcc.scala`（`DecoupledCluster`）、`dcc-mem.scala`（`VGU`、`VPU`、`VSU`、`VLU`、`VLUMapper`）、`dcc-fu.scala`（`VDU`、`VDUCtrl`）

## 1. 为什么需要解耦簇

systolic bank 数据通路只能和**定延迟**的单元配合。访存和除法的延迟不可预知，因此需要一层队列把它们与 bank 隔开：bank 侧把操作数/谓词压进队列就算完成，解耦簇再按下游的节奏消费；反向的写回也先进队列，再择机写 VRF。DCC 就是这一层。

```
       bank 侧                          DCC                                  下游
 BPQ(每 bank) ──────▶ VPU ─────────────────────────────▶ VMU 谓词队列 VPQ
 LPQ/LRQ (VGU 口) ──▶ VGU ─────────────────────────────▶ VMU 虚地址队列 VVAQ
 BRQ(每 bank) ──────▶ VSU ─置换网络─────────────────────▶ VMU store 数据队列 VSDQ
 BWQ.mem(每 bank) ◀── VLU ◀─旋转/置换网络─ VLDQ ◀──────── VMU load 数据
 LPQ/LRQ (VFU2 口) ─▶ VDU ─▶ IDiv / FDiv / RPred / RFirst ─▶ BWQ.fu 或 标量单元
```

发射时 `IssueOp.active.enq_dcc()` 决定是否要同时向 DCC 入队一个 `DCCOp`（vlen、active 位图、fn、目的寄存器），DCC 再按 `enq_vgu/vpu/vsu/vlu/vdu` 分发到各子单元的 opq（深度 `nDCCOpQ = 2`）。所有相关子单元同时 ready 才 fire。

## 2. 序列器的 look-ahead 计数器

序列器发射一个 strip 前必须保证下游队列有空间，否则 systolic 通路会被迫停顿。DCC 为每个队列提供 `CounterLookAheadIO`：序列器在发射前**预约**（`dpla`、`dqla`、`gpla`、`gqla`、`pla`、`lla`、`sla`）若干条目，计数器减少；数据真正被消费后计数器恢复。这就是序列器"结构冒险"检查中队列容量部分的实现。

## 3. VGU（向量地址生成单元）

- 输入：LPQ（谓词）、LRQ（偏移值）各一条，深度 nBanks+2。
- 输出：VVAQ 条目（虚地址 = 由 VMU 加上基址；VGU 只送偏移与元素索引）。
- 每拍处理 1 或 2 个元素：谓词为空或只有一个活跃元素时可以一次处理 2 个（源码注释说明了这一 popcount 判断）。
- 用于索引 load/store 与 AMO 的地址来源。

## 4. VPU（向量谓词单元）

- 每 bank 一条 BPQ（深度 2×nBanks）。
- 把各 bank 读出的谓词按元素顺序拼接后送给 VMU 的谓词队列，VMU 用它判断哪些元素真正需要访问（生成地址掩码和 store 字节掩码，以及判断缺页是否真实）。
- 常量/单位步长访存都需要 VPU（它们不需要 VGU，因为地址由 VMU 自己生成）。

## 5. VSU（向量 store 数据单元）

- 每 bank 一条 BRQ（深度 4）加 look-ahead 计数器（`maxSLA = 7`）。
- 一条 predq（深度 `nDCCPredQ = 4`）跟踪掩码。
- **置换网络**把来自 4 个 bank × 2 slice 的元素重新排列成内存顺序，按元素宽度打包成 128 位 beat，写入 VMU 的 VSDQ。
- VMU 侧的 SBox 再做对齐（基址非 128 位对齐、常量步长、scatter、AMO）。

## 6. VLU（向量 load 数据单元）

VLU 是 DCC 中最复杂的单元，因为内存系统会**乱序**返回数据：

- `VLUMapper` 把每个 load 操作的目的寄存器映射到各 bank 的物理行，并计算旋转量。
- 一个 `Rotator` 置换网络把 128 位 beat 中的元素旋转到正确的 bank/slice 位置。
- 掩码/冲突仲裁：同一拍多个 beat 想写同一 bank 时仲裁。
- 每 bank 一条 BWQ（深度 2），写请求进入 BWQ 后由 bank 的写仲裁器写入 SRAM。
- **机会式写回**：VRF 允许元素乱序到达，VLU 用一个已退休元素的位向量（writeback status）跟踪进度，而不是用重排序缓冲。序列器的 VLU 簿记操作根据这个位向量递减 vlen，后续依赖该寄存器的操作据此放行。
- **多操作并发**：`nVLU = 2`，VLU 可同时管理两个 load 操作，避免 VMU 在连续 load 之间被人为节流。
- 源码注释提到一个竞争条件：短向量且谓词全空时，VLU 可能在 VCU 之前退休，因此在 predq 前额外加了一拍延迟。

## 7. VDU（向量解耦功能单元簇 / VFU2）

- `VDUCtrl` 从 opq 取操作，从 LPQ（谓词）和两条 LRQ（操作数，`nVDUOperands = 2`）取数。
- 例化 `IDivSlice`、`FDivSlice`、`RPredLane`、`RFirstLane`。
- 结果进入 tagq / 结果队列（深度 `nDecoupledUnitWBQueue`），再写入 BWQ.fu；`icntr`/`fcntr` look-ahead 计数器向序列器报告输出队列余量。
- 归约结果通过 `ReduceResultIO` 送到顶层的 RPredMaster / RFirstMaster。
- 序列器的 VIDU/VFDU 簿记操作对应这里的写回进度。

## 8. 关键参数

| 参数 | 值 |
|---|---|
| nDCCOpQ | 2 |
| nDCCPredQ | 4 |
| nVDUOperands | 2 |
| nBPQ | 2 × nBanks |
| nBRQ | 4 |
| nBWQ | 2 |
| maxSLA | 7 |
| nVLU | 2 |
