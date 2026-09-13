# Hwacha 向量加速器文档集

本目录整理了 UC Berkeley Hwacha 解耦向量取指（decoupled vector-fetch）加速器的设计思想、编程模型、ISA、整体架构、微架构以及各子模块的实现细节。内容依据以下一手资料整理，并对照 `ucb-bar/hwacha` 开源 Chisel 源码做了核对：

| 资料 | 编号 / 来源 |
|---|---|
| The Hwacha Vector-Fetch Architecture Manual, v3.8.1 | UCB/EECS-2015-262 |
| The Hwacha Microarchitecture Manual, v3.8.1 | UCB/EECS-2015-263 |
| Hwacha Preliminary Evaluation Results, v3.8.1 | UCB/EECS-2015-264 |
| Yunsup Lee 博士论文《Decoupled Vector-Fetch Architecture with a Scalarizing Compiler》 | UCB/EECS-2016-117 |
| Albert Ou 硕士论文《Mixed Precision Vector Processors》 | UCB/EECS-2015-265 |
| ESSCIRC 2014《A 45nm 1.3GHz 16.7 DP GFLOPS/W RISC-V Processor with Vector Accelerators》 | hwacha.org |
| Chipyard 文档 Hwacha 章节、`ucb-bar/hwacha` 源码 | GitHub |

所有文档描述的是 Hwacha 第四代架构（技术报告称 v3.8.1，即通往 v4 的快照），也是开源仓库实现的版本。

## 阅读路线

**想快速理解 Hwacha 是什么**：先读 `01-design-philosophy.md`，再读 `02-programming-model.md`。

**想写 Hwacha 汇编或编译器后端**：`02-programming-model.md` → `03-isa-reference.md` → `16-mixed-precision.md`。

**想读懂 RTL 或做性能分析**：`04-system-architecture.md` → `05-microarchitecture-overview.md` → `modules/` 目录按数据流顺序阅读 → `18-source-map.md`。

## 文档目录

### 总览

- [01-design-philosophy.md](01-design-philosophy.md) — 设计思想：为什么是"向量取指"，与 packed-SIMD、SIMT、传统向量机、vector-thread 的对比，六大架构特性
- [02-programming-model.md](02-programming-model.md) — 编程方法学：控制线程与工作线程、寄存器状态、stripmine 循环、代码示例、编译器映射、性能编程要点
- [03-isa-reference.md](03-isa-reference.md) — ISA 参考：控制线程指令、工作线程指令格式与全部指令类别、异常
- [04-system-architecture.md](04-system-architecture.md) — 整体架构：Rocket Chip SoC、RoCC 接口、TileLink 与 L2、Hwacha 机器组织
- [05-microarchitecture-overview.md](05-microarchitecture-overview.md) — 微架构总览：一条向量指令的生命周期、strip、systolic bank 执行、chaining、多 lane

### 子模块（按数据流顺序）

- [modules/06-rocc-unit.md](modules/06-rocc-unit.md) — RoCC 单元与命令队列
- [modules/07-scalar-unit.md](modules/07-scalar-unit.md) — 标量单元、标量访存单元（SMU）、共享 FPU 接口
- [modules/08-vru.md](modules/08-vru.md) — 向量预取单元（VRU）
- [modules/09-sequencer.md](modules/09-sequencer.md) — 主序列器与 lane 序列器
- [modules/10-expander.md](modules/10-expander.md) — 展开器（expander）与 bank 微操作
- [modules/11-lane-bank-vrf.md](modules/11-lane-bank-vrf.md) — 向量 lane、bank、向量/谓词寄存器堆、操作数交叉开关
- [modules/12-vfu.md](modules/12-vfu.md) — 向量功能单元：ALU、PLU、FMA、IMul、FConv、FCmp、FDiv/FSqrt、IDiv、归约
- [modules/13-dcc.md](modules/13-dcc.md) — 解耦簇（DCC）：VGU、VPU、VSU、VLU、VDU
- [modules/14-vmu.md](modules/14-vmu.md) — 向量访存单元（VMU）：IBox、ABox、PBox、SBox、MBox、TLB、MRT
- [modules/15-memory-ordering-exceptions.md](modules/15-memory-ordering-exceptions.md) — 内存排序单元、fence、虚拟内存与可重启异常

### 专题

- [16-mixed-precision.md](16-mixed-precision.md) — 混合精度扩展（HOV / MXP）
- [17-evaluation-and-chips.md](17-evaluation-and-chips.md) — 评估结果、流片历史与 VLSI 数据
- [18-source-map.md](18-source-map.md) — 开源代码结构与参数速查
- [19-toolchain-and-usage.md](19-toolchain-and-usage.md) — 工具链、Chipyard 集成、测试与仿真
- [20-history-references.md](20-history-references.md) — 谱系、历史与外部资源清单
- [21-glossary.md](21-glossary.md) — 术语与缩写表
- [22-performance-model.md](22-performance-model.md) — Python 性能模型：抽象层次、建模机制、内核格式、验证结果与局限
- [23-cpp-model.md](23-cpp-model.md) — C++ 事件驱动周期级模型（gem5 风格）：框架、精度提升、与 Python 模型的对比
