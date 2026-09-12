# 22 Hwacha 性能模型

代码位于仓库根目录的 `hwacha_perf/`。本文说明模型的抽象层次、建模的微架构机制、输入格式、输出指标、已验证的性质与已知局限。

## 1. 模型定位

模型有两层：

| 层 | 模块 | 用途 | 精度 |
|---|---|---|---|
| 解析式上下界 | `analytic.py` | 秒级估计；给出每种资源（读口、FMA、访存端口、L2、DRAM、标量发射、控制线程）的周期下界与最紧的瓶颈 | 下界，忽略依赖与延迟 |
| strip 粒度周期模拟 | `sim.py` | 模拟指令生命周期、序列器窗口、chaining、功能单元/端口冲突、访存延迟与带宽、VRU 预取 | 周期近似（一阶效应） |

两者共用同一份内核描述与配置，模拟结果应当不低于解析下界，通常在下界的 1–1.3 倍以内。

## 2. 建模的微架构机制

模型按 `docs/05-microarchitecture-overview.md` 中的路径逐段建模：

**控制线程（Rocket）**：每次 stripmine 迭代发出 `vsetvl`、若干 `vmca`/`vmcs`、`vf`，每条命令 1 周期，另加可配置的簿记周期（`@ctrl`，默认 6）；命令进入深度 32 的 VCMDQ，队列满则停顿。同一命令流也进入 VRCMDQ 供 VRU 使用。

**标量单元**：处理命令（1 周期/条），`vf` 后经取指延迟逐条译码工作线程指令（1 条/周期）。标量指令本地执行；标量访存/浮点/乘除有固定延迟并用 scoreboard 互锁共享寄存器；`vfence` 等待所有在途访存；一致性分支等待谓词归约操作完成后按 `# @taken N` 决定跳转。向量指令需要主序列器有足够空槽（按 `docs/modules/09-sequencer.md` 的槽位表：算术 1、访存 3、AMO 4、除法 2），否则停顿（`seq_full`）。

**主序列器 / lane 序列器**：每条向量指令按 8 元素 strip 分配到各 lane（lane 间按 strip 交错）。每 lane 每周期最多发射一个 strip，选择最老的无冒险操作。冒险按 strip 粒度检查：
- RAW：生产者对应 strip 的写回时刻 < 当前时刻（自然形成 chaining）；来自访存的 RAW 单独统计为 `raw_mem`；
- WAR：生产者对应 strip 已读；
- WAW：本 strip 的写回时刻晚于旧操作的写回。

**展开器 / systolic bank**：用资源时间线表示：SRAM 读口每 strip 占用 `rp` 个周期（`rp` = 向量源操作数个数），谓词读口 1 周期，共享功能单元在 `t+rp+1` 起占用 `nBanks` 个周期（bank 逐拍送入操作数），操作数锁存器组按 `docs/modules/10-expander.md` 第 6 节的分配占用 `[t+1, t+rp+1]`，写口在 `t+rp+1+延迟` 占用 1 周期。两个 FMA 簇择空闲者使用。ALU/PLU 为 bank 本地，只受读写口限制。

**变延迟单元**：除法/开方/整数除按元素串行占用单个单元（每元素周期数可配），结果经 BWQ 写回。`vfirst` 与谓词归约需要 `nBanks + 归约延迟` 后才把结果送回标量单元。

**VMU（每 lane）**：访存操作按发射顺序处理；发射后经 `vmu_issue_latency` 开始，每周期最多发出一个 16 字节 beat；在途 beat 不超过 `n_vmt_entries`（64）；每页首次访问付 TLB 缺失延迟（8 项 LRU TLB）。单位步长按 strip 元素范围合并成 beat，常量步长与索引访存每元素一个 beat（相邻元素同 beat 时合并）。store 的 beat 需要等 VSU 把该 strip 的数据读出（VSDQ 深度限制 VSU 领先量）；索引访存需要 VGU 先读出索引。load 数据按 beat 返回，一个 strip 的所有 beat 返回后经 `vlu_latency` 写回（占用写口一次），并受 WAW/WAR 门控。lane 的响应端口每周期只能回一个 beat。

**内存系统（共享）**：L2 分 bank（按 64 B 行交错）、组相联 LRU、写分配；每 bank 每周期接受一个请求；每 bank 在途缺失不超过 `l2_trackers_per_bank`。缺失走 DRAM：通道按行交错，每通道有固定延迟与带宽（默认 LPDDR3-933 ×32 双通道 ≈ 7.5 B/周期）。脏行替换占用通道带宽。

**VRU**：处理 VRCMDQ 中的 `vsetvl`/`vmca`/`vf`；对每个 vf 块以 1 条/周期译码，对以 va 为基址的单位/常量步长访存计算触及的行并入预取队列；每周期最多发一个预取，在途不超过 20；跳过前 1 个块；用"已译码但 VXU 尚未完成的块字节数"做领先距离节流；块完成由主序列器应答。

## 3. 参数

`HwachaConfig` 与 `MemoryConfig`（`hwacha_perf/config.py`）的字段名与 `docs/18-source-map.md` 中的 Chisel 参数一一对应，可用 `--set key=val`、`--set mem.key=val` 或 JSON 配置覆盖。`configs/` 下提供：

| 文件 | 含义 |
|---|---|
| `paper-28nm.json` | 论文评估配置（单 lane、VRU、4×256 KB L2、LPDDR3 双通道） |
| `paper-28nm-mxp.json` | 同上并启用混合精度 |
| `open-source.json` | 开源主线：无 VRU、broadcast hub 代替 L2（单 bank、小容量、无 AMO） |
| `fast-memory.json` | 理想内存，用于隔离计算侧瓶颈 |

## 4. 内核文件格式

向量取指块用 Hwacha 汇编书写（支持 `@[!]vpN` / `!vpN` 谓词前缀、`@s`、`@all`），控制线程用 `# @` 指令描述：

| 指令 | 含义 |
|---|---|
| `@n N` | 应用向量长度（可被 `--n` 覆盖） |
| `@cfg v64= v32= v16= vp=` | vsetcfg，决定 HVL |
| `@array name elem=B n=N [base=0x..]` | 数组，自动分配页对齐基址 |
| `@va vaK = array [stride=E] [offset=E] [fixed]` | 指针型地址寄存器；每次迭代前进 vl×elem×stride，越界回绕 |
| `@va vaK = 4096` | 常量（如字节步长） |
| `@vs N` | 每次迭代 vmcs 条数 |
| `@ctrl N` | 每次迭代控制线程簿记周期 |
| `@iters N` | 覆盖迭代次数 |
| 指令行 `# @taken N` | 一致性分支每次 vf 被采纳的次数 |
| 指令行 `# @gather array [random\|unit]` | 索引访存的目标数组与访问模式 |

## 5. 输出指标

`run` 输出：总周期、每元素周期、GFLOPS（按 FMA 元素数 ×2）、FMA 利用率、读口利用率、访存带宽（B/周期）、序列器平均占用；lane 状态分解（`issue`、`raw`、`raw_mem`、`war`、`waw`、`rport`、`fu`、`latch`、`wport`、`vsdq`、`drain`、`empty`）、标量单元状态（`issue`、`seq_full`、`scoreboard`、`branch`、`fence`、`idle`）、VMU 状态（`busy`、`vmt_full`、`tlb`、`wait_data`、`wait_addr`、`idle`）、控制线程状态（`issue`、`bookkeeping`、`vcmdq_full`、`done`）、L2/DRAM 统计与 VRU 统计。`--json` 输出机器可读格式，`--bounds` 附带解析下界。

## 6. 已验证的性质（tests/）

| 性质 | 结果 |
|---|---|
| 寄存器内 FMA 链（2 向量源 + 1 标量）达到双 FMA 簇峰值 | 单 lane 8.0 GFLOPS @1 GHz，FMA 利用率 100%，读口利用率 100% |
| 流式内核（vvadd/daxpy）受 DRAM 带宽限制 | 模拟周期在解析下界的 1.03 倍以内；解析模型判定 `dram_bandwidth` 为瓶颈 |
| VRU 预取对流式内核有效 | daxpy 32 K 元素：开启 70.8 k 周期，关闭 89.6 k 周期，预取命中覆盖全部行 |
| 多 lane 对计算受限内核有效 | dgemm 分块 131 K 元素：1/2/4 lane FMA 利用率 99.5% / 92.8% / 77.7%；理想内存下 4 lane 达 98.6% |
| 混合精度使 32 位内核 HVL 翻倍、strip 数减半 | saxpy 在 `conf_prec` 下 maxvl 2048 vs 1024 |
| 序列器槽位限制发射窗口 | vvadd 在 8 槽时标量单元 `seq_full` 占比高于 16 槽 |
| 索引访存每元素一个 beat | gather 内核 beat 数与元素数一致（相邻元素同 beat 合并除外） |
| 一致性分支循环按注解次数执行 | `@taken 3` 时块内操作数为 1 + 4×3 + 1 |

`HVL` 计算与 `rocc-unit.scala` 的 epb 逻辑一致（例如 6 个 64 位寄存器 → 42 行 → HVL 336；16 个谓词寄存器把 HVL 限制到 128）。

## 7. 典型结论（默认论文配置，n = 16 K）

`python3 scripts/summary.py --n 16384` 的输出（论文配置 vs 开源配置，单 lane）：

| kernel | config | cycles | cyc/elem | GFLOPS | FMA util | B/cycle | 解析下界 | 瓶颈 |
|---|---|---|---|---|---|---|---|---|
| vvadd | paper-28nm | 35685 | 2.178 | 0.92 | 11.5% | 11.02 | 35140 | dram_bandwidth |
| vvadd | open-source | 67049 | 4.092 | 0.49 | 6.1% | 5.86 | 32768 | dram_bandwidth |
| daxpy | paper-28nm | 35685 | 2.178 | 0.92 | 11.5% | 11.02 | 35140 | dram_bandwidth |
| saxpy | paper-28nm | 17905 | 1.093 | 1.83 | 22.9% | 10.98 | 17570 | dram_bandwidth |
| csaxpy | paper-28nm | 20104 | 1.227 | 1.63 | 20.4% | 11.41 | 19767 | dram_bandwidth |
| sfilter | paper-28nm | 24026 | 1.466 | 4.09 | 51.1% | 13.64 | 18077 | dram_bandwidth |
| dgemm_opt | paper-28nm | 34200 | 2.087 | 7.67 | 95.8% | 7.67 | 32800 | vrf_read_port |
| dgemm_opt | open-source | 64930 | 3.963 | 4.04 | 50.5% | 4.04 | 32800 | vrf_read_port |
| gather | paper-28nm | 50077 | 3.056 | – | – | 10.47 | 39533 | dram_bandwidth |
| fma_peak | paper-28nm | 32269 | 1.970 | 8.00 | 100.0% | 0.00 | 32256 | vrf_read_port |

（cyc/elem 对 fma_peak 按 @iters=32 的元素数折算；gather 无浮点运算。）

- 访存密集内核（vvadd、daxpy、saxpy、csaxpy、sfilter）在 LPDDR3 双通道下都是 DRAM 或 lane 端口受限，FMA 利用率 10%–50%，与论文"*axpy 受访存限制"一致。
- 8 槽序列器且访存占 3 槽，使得 `2 load + 1 FMA + 1 store` 的块无法整体驻留窗口，标量单元大部分时间处于 `seq_full`。把 `n_seq_entries` 提到 16 可以消除这一停顿，但流式内核仍被 DRAM 限制。
- 64 项 VMT 在 110 周期 DRAM 延迟下把单 lane 的未预取访存吞吐限制在约 9 B/周期，这正是 VRU 的价值所在；VRU 自身 20 项在途预取上限又把预取带宽限制在约 11.6 B/周期，与双通道 LPDDR3 大致匹配。
- 随机 gather 在 8 项 DTLB 下一旦表大于 32 KB 就被 TLB 缺失主导（每元素约一次 40 周期的 PTW），这是模型给出的一个值得注意的微架构特性。
- 开源配置（`open-source.json`）由于没有 VRU 且只有单 bank 小 L2，流式内核性能比论文配置差 20%–40%，与 Ara 论文对开源版 Hwacha 的批评方向一致。

## 8. 已知局限

- 不模拟 Rocket 核本身的流水线，控制线程只按命令计数与固定簿记周期建模。
- 每 lane 每周期最多发射一个序列器操作（未建模源码中的第二调度端口）；VPU 谓词读出对访存视为免费。
- VMU 内地址流水线按操作严格顺序，未建模 IBox 的双操作并发与 VLU 同时管理两个 load 的细节；分段访存按扩大的元素宽度近似。
- L2 用"下一空闲时刻"队列模型而非逐周期仲裁；未建模一致性协议对 L1D 的探测、L2 内 AMO ALU 的占用。
- 内存排序单元只体现在 fence 上，未模拟跨 lane store 的串行化。
- 混合精度只影响 HVL、strip 元素数与吞吐，不模拟子字打包 mux 的额外延迟。
- 可重启异常、上下文切换、TLB 缺页处理不在范围内。
- 所有延迟参数（L2、DRAM、PTW、除法器）为文献校准值或估计值，没有用 RTL 仿真做逐周期校准；模型用于比较与趋势分析，不用于给出绝对周期精度。
