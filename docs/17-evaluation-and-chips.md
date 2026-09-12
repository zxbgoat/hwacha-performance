# 17 评估结果、流片历史与 VLSI 数据

## 1. 流片时间线

| 时间 | 事件 | 工艺 | 说明 |
|---|---|---|---|
| 2011 | Hwacha 项目启动（接续 Maven） | – | v1 用 Verilog |
| 2014-03 | EOS22 流片 | IBM 45 nm SOI | 双核 Rocket + Hwacha，ESSCIRC 2014 论文 |
| 2014-09 | ESSCIRC 2014 发表 | – | 1.3 GHz @1.2 V，16.7 DP GFLOPS/W @0.65 V |
| 2014-11 | Raven3.5 流片 | ST 28 nm FD-SOI | 集成开关电容 DC-DC 与自适应时钟 |
| 2015-12 | v3.8.1 三份技术报告 | – | 架构、微架构、评估 |
| 2016 | Hurricane 流片 | ST 28 nm FD-SOI | 亚微秒自适应电压调节 |
| 2016 | Yunsup Lee 博士论文 | – | 第四代架构完整文档 |
| 2021 | JSSC《An eight-core 1.44-GHz RISC-V vector processor in 16-nm FinFET》 | TSMC 16 nm | Hwacha 谱系的多核向量处理器 |

知乎文章称从 2011 年起几乎每年流片，到 v4 已是第 14 款芯片。

## 2. EOS 芯片（ESSCIRC 2014）

- 64 位双核 RISC-V，每核一个 Rocket 标量核 + 一个 Hwacha 向量加速器，16 KB L1I、32 KB L1D（标量与向量共享）、8 KB L1 VI$，1 MB 片上 SRAM 阵列作为主存，广播式 MESI 一致性 hub。
- 面积 3 mm²，最高 1.3 GHz @ 1.2 V，峰值能效 16.7 双精度 GFLOPS/W @ 0.65 V。
- 向量加速器能效为同工艺 IBM Blue Gene/Q 的 1.8 倍、IBM Cell 的 2.6 倍。
- Rocket 在 TSMC 40 nm 下 1.72 DMIPS/MHz，比 Cortex-A5 高 10%，面积效率高 49%。
- 该版本 Hwacha：单 lane，8 个 64 位 1R1W SRAM bank，per-bank 整数 ALU，浮点单元与 Rocket 共享，向量访存指令仍在标量流中。

## 3. v3.8.1 / 博士论文的评估框架

- RTL 用 Chisel 编写，Synopsys DC + ICC 在商用 28 nm HKMG 类工艺下综合与布局布线，10 层金属用 8 层布线。一周物理设计迭代约 100 个版图，单 lane 频率提升约 50%。
- 门级仿真跑 OpenCL 微基准得到周期数与翻转率，PrimeTime PX 结合寄生参数得到功耗，能耗 = 平均功耗 × 运行时间。
- 对标平台：Samsung Exynos 5422 上的 ARM Mali-T628 MP6（600 MHz，6 个 shader core，分为 2 核与 4 核两个 OpenCL 设备，称 Mali2/Mali4），ODROID-XU3 板可分轨测功耗。一个 Mali MP 的算术吞吐约等于一个带混合精度的 Hwacha lane。
- 内存系统用 ccbench 校准：L1 约 4 周期，L2 约 22 周期，LPDDR3 约 110 ns，与 Exynos 5422 一致。
- 微基准：`{s,d,hs,sd}axpy`、`gemm`、`gemm-opt`（手工优化）、`filter`（高斯模糊）、`mask-filter`（带谓词），四种精度组合。
- 编译器：基于 PoCL + 自定义 LLVM 后端的标量化 OpenCL 编译器，离线编译并录制内核输入输出以缩短仿真。

## 4. VLSI 质量结果

| | 基线 1L (PNR) | 基线 1L | 2L | 4L | MXP 1L (PNR) | MXP 1L | 2L | 4L |
|---|---|---|---|---|---|---|---|---|
| 面积 mm² | 2.11 | 2.23 | 2.60 | 3.59 | 2.21 | 2.32 | 2.82 | 4.04 |
| 周期 ns | 0.90 | 0.95 | 0.93 | 0.93 | 0.94 | 0.98 | 1.02 | 1.08 |
| 频率 GHz | 1.11 | 1.05 | 1.08 | 1.08 | 1.06 | 1.02 | 0.98 | 0.93 |

（面积含 Rocket + L1 与 256 KB L2；2/4 lane 仅综合结果。）目标频率 1.2 GHz 以匹配 Mali2 的功能单元带宽，实际略低但普遍高于 1 GHz。功能单元、bank、控制、VMU 面积随 lane 数线性增长。

面积效率估算：Exynos 5420 的 Mali T628 MP6 含 192 KB L2 约 30 mm²（28 nm）；按论文面积分布外推，6 lane Hwacha + 256 KB L2 约 6.2 mm²。即使保守假设图形专用硬件占 Mali 一半面积，Hwacha 仍有约 3 倍面积效率优势。

## 5. 性能与能耗结论

- `*axpy`：Mali2/Mali4 比 Hwacha 快 1.5–2 倍，也是唯一一组 Mali 稳定领先的基准，推测源于外层内存系统（如访存调度器）建模差异。
- `*gemm`：Mali2 比 Hwacha 基线慢 3–4 倍，Mali4 快 Mali2 约 2 倍但仍慢于 Hwacha，推测工作集放不进 Mali 缓存，不完全公平。
- `*filter`：Mali2 约为 Hwacha 一半，Mali4 与 Hwacha 相当。
- OpenCL 版本与手写汇编差距明显，原因是手写代码跨向量块保留数据在 VRF 中。
- 能耗：Hwacha 以 1 V 过驱动达 1 GHz，Mali 为 0.9 V；`*axpy` 上 Mali 能效约为 Hwacha 两倍；降精度可以一致地降低能耗。
- 注意所有比较都是**单 lane** Hwacha 对 2 核/4 核 Mali，对 Hwacha 不利。

## 6. 第三方复现与批评

- Ara（ETH，2019）：开源 Hwacha 只有 L2 broadcast hub，无 banked cache，等效带宽每周期 128 位，FMA 饥饿，论文里 128×128 MATMUL 95% FPU 利用率的数据无法在开源版复现。
- Chipyard 文档明确 Hwacha 不实现 RVV。
- hwacha-template 已于 2020 年归档，AMO 测试在示例配置下预期失败。
