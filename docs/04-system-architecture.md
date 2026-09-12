# 04 整体架构

## 1. 系统层次

Hwacha 通过 Rocket Chip SoC 生成器集成到片上系统中。一个 **tile** 由 Rocket 控制处理器和一个 RoCC（Rocket Custom Coprocessor）插槽组成，Hwacha 及其向量指令缓存放在 RoCC 插槽内。

```
┌──────────────────────── Tile ────────────────────────┐
│  Rocket 控制核        RoCC 插槽                      │
│  (5 级顺序流水)  ───▶  Hwacha 向量加速器             │
│  L1I$   L1D$           L1 VI$ (4 KB, 2 路)           │
└───────┬───────────────────┬──────────────────────────┘
        │ cached TileLink   │ uncached TileLink（每 lane 128 位）
        ▼                   ▼
   ┌────────── L1-to-L2 TileLink 交叉开关 ──────────┐
   │  L2 Bank0   L2 Bank1   L2 Bank2  …  L2 BankN  │  分 bank、组相联、包含 L1
   └───┬───────────┬───────────┬──────────────┬─────┘
       │ TL/AXI4   │           │              │
   ┌───────────── AXI4 交叉开关 ───────────────┐
   │ LPDDR3 Ch0   LPDDR3 Ch1  …  LPDDR3 ChN  │  DRAMSim2 建模
   └──────────────────────────────────────────┘
```

**Rocket**：5 级（ESSCIRC 版本为 6 级）单发射顺序 RISC-V 核，阻塞式 L1I 与非阻塞 L1D，支持页式虚拟内存，可启动 Linux。Hwacha 的控制线程指令在 Rocket 的 commit 阶段、所有异常清除后才发送给 Hwacha。

**共享 L2**：分 bank、组相联、完全包含 L1。地址按 cache line 粒度在 bank 间交错。L2 bank 是一致性协议的 master 端点，可在 tag 阵列中放目录位加速协议。

**TileLink**：两种接口。cached TileLink 供持有私有副本的客户端（L1D、L2 bank）使用；uncached TileLink 供不持有副本的客户端使用，包括所有指令缓存和 Hwacha 向量单元。

## 2. 为什么向量单元直连 L2 而不是 L1D

- Hwacha v3 之前 VMU 接 L1D；v3 起改为直接与 L2 对话。
- 收益：每 lane 独立的 128 位接口带来远高于 L1D 的带宽；L2 支持子块访问（有利于 gather/scatter）、预取合并与 bank 内 AMO ALU。
- 代价：平均延迟更长，而且访存合并（coalescing）成为 VMU 的关键功能。延迟靠解耦和 VRU 预取补偿。
- 一致性：L2 用目录位判断某行是否在 L1D 中；向量单元读时若 L1D 持有 exclusive 副本，L2 先让其降级为 shared；向量单元写时让 L1D 丢弃该行。因此控制线程与工作线程共享同一个虚拟地址空间，无需 host/device 两套内存。

## 3. Hwacha 机器组织

```
                    ┌───────────────── Hwacha 解耦向量加速器 ─────────────────┐
 Rocket ──RoCC──▶ RoCC 单元 ──VCMDQ──▶ 标量单元 (SXU) ──▶ 主序列器 ──┬─▶ Lane 0: VXU + VMU
                      │                 │  ▲ FPREQQ/FPRESPQ            ├─▶ Lane 1: VXU + VMU
                      │                 │  │ (Rocket FPU)              ├─▶ …
                      │                 ▼  │                           └─▶ Lane N: VXU + VMU
                      │               SMU ─┼──────────────────────────────▶ L2
                      └────VRCMDQ──▶ VRU ──┼── 预取 ──────────────────────▶ L2
                                           │
                                  4 KB L1 VI$ (标量单元与 VRU 各一个端口)
```

各部分职责：

| 单元 | 职责 | 文档 |
|---|---|---|
| RoCC 单元 | 接收控制线程指令，计算 HVL，分发到 VCMDQ / VRCMDQ，应答 vsetvl/vgetcfg/vgetvl | modules/06 |
| 标量单元（SXU） | 取指译码向量取指块；本地执行标量整数指令；标量访存交给 SMU；标量浮点交给 Rocket FPU；向量指令交给主序列器 | modules/07 |
| SMU | 标量 load/store，直接访问 L2 | modules/07 |
| VRU | 独立取指向量块，解码常量步长访存，向 L2 发预取 | modules/08 |
| 主序列器 | 全局的向量指令窗口，保存依赖信息，向所有 lane 分发工作，跟踪进度，退休 | modules/09 |
| VXU（每 lane） | lane 序列器 + 展开器 + 4 个 bank 的 VRF/PRF + 功能单元 + 解耦簇 | modules/09–13 |
| VMU（每 lane） | 地址生成、翻译、合并、store 对齐、load 回写、TileLink 接口 | modules/14 |
| 内存排序单元（MOU） | 实现 fence 语义，跟踪各单元未完成的访存 | modules/15 |
| RPredMaster / RFirstMaster | 跨 lane 汇总谓词归约（一致性分支）和 vfirst | modules/12 |

开源代码中的顶层 `Hwacha` 是一个 `LazyRoCC`，占用 custom-0 与 custom-1 操作码，申请 2 + nLanes 个 PTW 端口（指令缓存、SMU、每 lane 一个 DTLB），并声明使用 FPU。

## 4. 三个解耦层次

1. **向量取指解耦**：控制线程只发 PC，标量核可以提前完成后续 stripmine 迭代的地址计算等簿记。VCMDQ 深度（默认 32）决定控制线程可以跑多远。
2. **访存/执行解耦**：VRU（访存处理器）从 L1 VI$ 独立取指，只解码单位步长/常量步长访存，配合 va 寄存器的只读约束发出非投机预取。VRU 不依赖 VXU 的任何决策，因此无死锁风险。
3. **lane 解耦**：主序列器同步向所有 lane 发射，但每个 lane 有自己的 lane 序列器，独立步进。lane 之间自然滑移以适应内存系统行为；只有归约操作（一致性分支、vfirst）需要主序列器把各 lane 同步起来。

## 5. 关键参数（默认值）

| 参数 | 默认 | 说明 |
|---|---|---|
| HwachaNLanes | 1 | lane 数，须为 2 的幂 |
| HwachaNBanks | 4 | 每 lane bank 数 |
| HwachaBankWidth | 128 | bank 位宽 |
| HwachaNSRAMRFEntries | 256 | 每 bank SRAM 条目 |
| HwachaMaxVLen | 4×256×128/64 = 2048 | 单 lane 最大 HVL |
| HwachaNSeqEntries | 8 | 序列器槽数 |
| HwachaCMDQLen | 32 | 命令队列深度 |
| HwachaNDTLB / NPTLB | 8 / 4 | 数据 TLB / 预取 TLB 条目 |
| HwachaNVMTEntries | 64 | 向量访存表（在途请求）条目 |
| HwachaBuildVRU | true | 是否例化 VRU |
| HwachaConfPrec | false | 是否启用混合精度 |

完整参数见 `18-source-map.md`。注意：当前开源代码中 `confvru` 与 `local_sfpu` 被硬编码为 false（源码注释：TODO，预取器需要适配 TileLink2 Hint），也就是说仓库主线的 VRU 实际未接入，预取效果无法在开源版本上复现。

## 6. 评估时使用的系统配置

论文对标 Samsung Exynos 5422（ARM Mali-T628 MP6）时选择：

| 组件 | 配置 |
|---|---|
| Hwacha | 基线 / 混合精度，1/2/4 lane |
| L1 VI$ | 4 KB，2 路 |
| Rocket L1D / L1I | 16 KB，4 路 |
| L2 | 4 bank × 256 KB（论文表 9.2 写 64 KB/bank），8 路，MESI + 目录位 |
| DRAM | DRAMSim2，LPDDR3 933 MHz，2 通道 × 1 GB |

ccbench 验证显示该模拟系统与 Exynos 5422 的 L1（约 4 周期）、L2（约 22 周期）、DRAM（约 110 ns）延迟基本一致，差别主要在 Cortex-A15 有硬件流预取器。
