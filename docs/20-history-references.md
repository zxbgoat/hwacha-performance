# 20 谱系、历史与外部资源

## 1. 名字

Rocket 是标量核，Hwacha（화차，火车）是朝鲜古代的多管火箭发射车，取"并行发射"之意。

## 2. 谱系

| 项目 | 时间 / 机构 | 关键特征 | 对 Hwacha 的影响 |
|---|---|---|---|
| T0 (Torrent-0) | 1992，UC Berkeley + ICSI，Krste Asanović | MIPS-II 基础向量处理器，HP 1.0 µm，45 MHz；16 个向量寄存器 × 32 元素，8 lane，5R3W 定制寄存器堆 | 传统向量机路线、Asanović 博士论文中的解耦向量流水与虚拟处理器宽度 |
| Scale | 2000，MIT，Krashinsky & Batten | vector-thread：SMIPS 控制核 + 4 lane × 4 cluster，原子指令块（AIB），do-across 网络，cache refill/access 解耦，180 nm 260 MHz | 向量取指块思想、refill/access 解耦 |
| Maven | 2007，UC Berkeley + MIT，Batten 主导，Yunsup Lee 设计 VT 单元 | 可配置 1/2/4 lane、1/2/4 路 banked VRF，向量算术提升到 vf 块，块内允许分支（硬件 PVFB 管理分歧），65 nm 版图未流片 | 直接前身；论文以其局限（无标量操作数、隐式分歧）为出发点 |
| Hwacha v1 | 2011 | 沿用 Maven 编程模型，改用 RV64；8 bank 1R1W SRAM 寄存器堆；Verilog | |
| Hwacha v2 | – | 早期 Chisel 重写；加入虚拟内存与可重启异常；VRU 改为预取进最近的 cache | |
| Hwacha v3 | – | Chisel 再次重写，控制逻辑与 VMU 更清晰；VMU 改接 L2 | |
| Hwacha v4 (v3.8.1) | 2015–2016 | 全部向量指令进 vf 块；全谓词 + 一致性分支；4 × 128 位 bank；归约、除法、开方硬件支持；混合精度 | 本文档集描述的版本 |

## 3. 主要贡献者（博士论文致谢）

- Yunsup Lee：首席架构师，ISA、架构、微架构、RTL、编译器、验证、微基准。
- Colin Schmidt：ISA 定义、标量单元 RTL、C++ 功能模拟器、torture 向量测试生成器、GNU 工具链 Hwacha 扩展、OpenCL 编译器与基准。后续博士论文《Extending Temporal-Vector Microarchitectures for Two-Dimensional Computations》（2021）。
- Albert Ou：VMU RTL、混合精度扩展（硕士论文）。
- Sagar Karandikar：bar-crawl 设计空间探索工具、VLSI 布图、VRU RTL、Mali GPU 评估、汇编微基准。
- Howard Mao：微架构手册合著。
- Palmer (Daniel) Dabbelt：评估报告合著。
- John Hauser：硬件浮点单元（hardfloat）。
- Andrew Waterman：架构手册合著、RISC-V 基础 ISA。
- Krste Asanović：导师与项目负责人。

## 4. 资金

Par Lab（Microsoft、Intel、UC Discovery）、DARPA POEM、ASPIRE Lab（DARPA PERFECT、C-FAR/SRC）、NVIDIA 研究生奖学金。

## 5. 一手资料

| 资料 | 链接 |
|---|---|
| 项目主页 | http://hwacha.org/ |
| 架构手册 v3.8.1 (EECS-2015-262) | https://www2.eecs.berkeley.edu/Pubs/TechRpts/2015/EECS-2015-262.pdf |
| 微架构手册 v3.8.1 (EECS-2015-263) | https://people.eecs.berkeley.edu/~krste/papers/EECS-2015-263.pdf |
| 评估报告 v3.8.1 (EECS-2015-264) | https://www2.eecs.berkeley.edu/Pubs/TechRpts/2015/EECS-2015-264.pdf |
| Yunsup Lee 博士论文 (EECS-2016-117) | https://people.eecs.berkeley.edu/~krste/papers/EECS-2016-117.pdf |
| Albert Ou 硕士论文 (EECS-2015-265) | https://www2.eecs.berkeley.edu/Pubs/TechRpts/2015/EECS-2015-265.pdf |
| Colin Schmidt 博士论文 (2021) | https://www2.eecs.berkeley.edu/Pubs/Dissertations/Years/2021.html |
| ESSCIRC 2014 论文 | https://people.eecs.berkeley.edu/~krste/papers/eos18-esscirc2014.pdf |
| RISC-V Workshop 幻灯片 (Schmidt & Ou) | https://content.riscv.org/wp-content/uploads/2018/12/Hwacha-A-Data-Parallel-RISC-V-Extension-and-Implementation-Schmidt-Ou-.pdf |
| Raven 28 nm 论文 | https://www.researchgate.net/publication/307800118 |
| JSSC 2021 16 nm 八核向量处理器 | C. Schmidt et al., IEEE JSSC 57(1), 2021 |
| Krste Asanović 发表列表 | https://people.eecs.berkeley.edu/~krste/publications.html |

## 6. 代码仓库

| 仓库 | 说明 |
|---|---|
| https://github.com/ucb-bar/hwacha | Chisel 微架构生成器 |
| https://github.com/ucb-bar/hwacha-template | 独立模板（2020 归档） |
| https://github.com/ucb-bar/esp-tools | 工具链元仓库 |
| https://github.com/ucb-bar/esp-isa-sim | Spike 分支 |
| https://github.com/ucb-bar/esp-opcodes | 指令编码 |
| https://github.com/ucb-bar/esp-tests | 测试与 benchmark |
| https://github.com/ucb-bar/esp-llvm | 早期 LLVM 分支 |
| https://github.com/ucb-bar/riscv-benchmarks/tree/master/hwacha | 向量 benchmark |
| https://github.com/ucb-bar/hwacha-net | 神经网络内核实验 |
| https://github.com/aignacio/hwacha_vvadd_benchmark | 第三方教学示例 |
| https://chipyard.readthedocs.io/en/stable/Generators/Hwacha.html | Chipyard 文档 |

## 7. 中文资料与讨论

| 资料 | 链接 |
|---|---|
| 知乎《RISCV的高性能计算探索：HWACHA的硬件架构解析》 | https://zhuanlan.zhihu.com/p/87038542 |
| CSDN《HWACHA_sequencer处理数据依赖性方法》 | https://blog.csdn.net/htluoxiaocheng/article/details/140093913 |
| RealWorldTech 论坛讨论 | https://www.realworldtech.com/forum/?threadid=157031 |
| Ara 论文（与 Hwacha 对比） | https://arxiv.org/pdf/1906.00478 |
| GitHub issue：Hwacha 是否支持 RVV | https://github.com/ucb-bar/hwacha/issues/25 |
| RISC-V hw-dev 邮件列表关于 Hwacha 源码的讨论 | https://groups.google.com/a/groups.riscv.org/g/hw-dev/c/M87YxPeDy1o |

## 8. 论文引用的关键先行工作

- J. E. Smith, "Decoupled Access/Execute Computer Architectures", 1984 —— 访存/执行解耦的原始提出。
- R. Espasa & M. Valero, "Decoupled vector architectures", HPCA 1996。
- C. Batten et al., "Cache Refill/Access Decoupling for Vector Machines", MICRO 2004。
- J. E. Smith, G. Faanes, R. Sugumar, "Vector instruction set support for conditional operations", ISCA 2000 —— VMU 边界情况处理引用。
- K. Asanović, "Vector Microprocessors", PhD thesis, 1998 —— T0 与解耦向量流水。
- Y. Lee et al., "Convergence and Scalarization for Data-Parallel Architectures", CGO 2013；"Exploring the Design Space of SPMD Divergence Management", MICRO 2014 —— 标量化编译器与谓词化的基础。
- Vaidyanathan et al., OS-friendly accelerator 框架 —— 可重启异常。
