# 14 向量访存单元（VMU）

源码：`vmu.scala`（`VMU`、`IBox`、`IBoxSL`、`IBoxML`）、`vmu-addr.scala`（`AGU`、`VVAQ`、`VPAQ`、`ABox0/1/2`、`ABox`）、`vmu-pred.scala`（`PBox0/1`、`PBox`）、`vmu-sdata.scala`（`SBox`）、`vmu-memif.scala`（`MBox`、`VMUTileLink`）、`vmu-tlb.scala`（`TBox`）、`vmu-table.scala`、`mrt.scala`（`MemTracker`）、`vector-unit.scala`

## 1. 位置与接口

每个 lane 有一个 VMU，拥有独立的 128 位 TileLink 接口直连 L2（uncached client，sourceId 数 = `HwachaNVMTEntries` = 64）。`VectorUnit` 把 VXU、VMU、`VMUTileLink`、`MemTracker` 和一个 Rocket `TLB`（`HwachaNDTLB` = 8 路）组装在一起。

VMU 与 VXU 之间有四条队列（论文图 8.6/8.10）：

| 队列 | 方向 | 内容 |
|---|---|---|
| VPQ | VXU→VMU | 谓词（来自 VPU） |
| VVAQ | VXU→VMU | 虚地址偏移（来自 VGU，索引访存/AMO） |
| VSDQ | VXU→VMU | store 数据（来自 VSU） |
| VLDQ | VMU→VXU | load 数据（去 VLU） |

## 2. 内部流水

```
标量单元 VMUOp ──▶ IBox (opq, 拆分连续段) ──┬──▶ ABox0 地址生成/翻译(TBox↔TLB) ──▶ VPAQ ──▶ ABox1 合并 ──▶ pipe ──▶ ABox2 元数据 ─┐
                                            │        ▲ VVAQ (索引偏移)                                                            │
                                            └──▶ PBox0 地址掩码 ──▶ anteq ──▶ PBox1 store 掩码 ──▶ PPQ ────────────────────────────┤
                                                     ▲ VPQ                                                                        ▼
                                              VSDQ ──▶ SBox 对齐 ──────────────────────────────────────────────────────▶ MBox (VMT 表, tag) ──▶ VMUTileLink ──▶ L2
                                              VLDQ ◀────────────────────────────────────────────────────────────────────┘ (load 数据 + tag → 元素索引)
```

**IBox（issue）**：`opq` 深度 `nVMUQ = 2`，保存操作类型、vlen、基址、步长。单 lane 用 `IBoxSL`；多 lane 用 `IBoxML`，把交错分配后本 lane 负责的元素拆成若干**连续段**（每段一个 strip 或 lane stride 长度），让后面的通路对多 lane 交错完全透明。`AGU` 在多 lane 时例化 2 个，用来把 lane 交错的基址计算当作短迭代乘法完成。

**ABox0（地址生成 + 翻译）**：常量步长访存在这里自己生成地址，不需要 VXU 参与；索引访存从 VVAQ 取偏移加基址。虚地址经 `TBox` 送 TLB 翻译，物理地址进入 `VPAQ`（深度 24）。单位步长访存按**页粒度**做初次生成和翻译，绕过谓词延迟并加快序列器的 VCU 检查。VPAQ 带 `vcucntr` look-ahead 计数器，对应序列器的 **VCU 簿记操作**：只有翻译成功的元素数达到一个 strip，后续 VSU/VLU 才放行，这是可重启异常的基础。

**ABox1（合并，coalescer）**：把相邻元素合并成一个 128 位请求，处理基址未对齐到 128 位和 vlen 不是打包密度整数倍的边界情况。

**ABox2（元数据生成）**：为每个请求生成 tag、字节掩码、元素索引范围等元数据，供 MBox 与 load 回程使用。

**PBox0 / PBox1（谓词通路）**：PBox0 从 VPQ 取谓词生成**地址掩码**，决定哪些元素真正需要访问，也用于判断缺页是否是真实缺页（被谓词屏蔽的元素缺页不算）。PBox1 生成 store 的**字节掩码**。VMU 对 2 的幂长度的连续假谓词做有限的 density-time skipping。

**SBox（store 对齐）**：从 VSDQ 取 128 位数据，按目标地址移位对齐（常量步长、scatter、AMO 的非理想对齐）。

**MBox（memory box）**：维护 `VMT`（vector memory table，64 项）记录每个在途请求的 tag → 元数据，把请求打包成 `VMUMemReq`；load 应答按 tag 查表恢复元素索引后送 VLDQ。

**VMUTileLink**：把 VMUMemReq 翻译成 TileLink A 通道消息（Get / PutPartial / 原子操作），D 通道应答回 MBox。源码注释提到并未总是请求最小的事务尺寸。

## 3. MemTracker（MRT）

每个 lane 一个（标量单元也有一个小的 `MemTracker(4,4)`）。用两个 `LookAheadCounter`（load 与 store，各 512）跟踪已发出但未完成的访存数：VXU 侧在序列器发射时预约（`lreq`/`sreq`），VMU 侧在应答返回时归还（`lret`/`sret`）。另有一个地址队列 `areq/aret` 记录正在发出 acquire 的序列器槽号。输出 `pending.{load,store,all,addr}` 给 MOU 与 fence 逻辑。

## 4. 访存类型的处理差异

| 类型 | 地址来源 | 谓词 | 序列器操作 |
|---|---|---|---|
| 单位步长 | IBox 基址 + 元素宽度，页粒度批量翻译 | VPU→VPQ | VPU + VCU + VSU/VLU |
| 常量步长 | IBox 基址 + 步长 × 索引 | VPU→VPQ | 同上 |
| 索引（gather/scatter） | vs 基址 + VVAQ 偏移 | 随 VGU 送入 LPQ | VGU + VCU + VSU/VLU |
| AMO | 同索引，数据走 VSDQ，结果走 VLDQ | 同上 | VGU + VCU + VSU + VLU |
| 分段（seg） | 同单位/常量步长，元素与寄存器号交错 | 同上 | 同上 |

## 5. 多 lane 下的行为

- 元素按 lane stride 交错，IBox 把本 lane 的部分拆成连续段，段与段之间在内存中有空洞。
- 若基址未对齐到 128 位，相邻 lane 会在段边界上请求同一个 TileLink beat（论文图 8.12，R2 与 R3 重叠等），浪费少量带宽。
- 每个 lane 的 VMU 各自有 TLB 和 TileLink 端口，因此 lane 数增加时访存带宽线性增加。

## 6. 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| HwachaNVVAQEntries | 4 | 虚地址队列 |
| HwachaNVPAQEntries | 24 | 物理地址队列 |
| HwachaNVSDQEntries | 4 | store 数据队列 |
| HwachaNVLDQEntries | 4 | load 数据队列 |
| HwachaNVMTEntries | 64 | 在途请求表 / TileLink source 数 |
| HwachaNDTLB | 8 | 数据 TLB 路数 |
| nVMUQ / nVMUIQ | 2 / 2 | IBox 操作队列 / 多 lane issue 队列 |
| nVMUPredQ | 4 | 谓词队列 |
| tlDataBytes | 16 | TileLink 数据宽度（128 位） |

## 7. 开源版本的内存系统限制

Ara 论文（ETH，2019）指出：论文中的 Hwacha 结果依赖 Berkeley 内部的 banked L2 cache，而开源版本只配了一个不支持 AMO 的 L2 broadcast hub，等效带宽被限制在每周期 128 位，FMA 单元会饥饿。hwacha-template 的 README 也说明 AMO 测试在示例配置下预期失败。用 Chipyard 集成时可以通过 `InclusiveCache`（SiFive L2）解决 AMO 与带宽问题。
