# 24 用 RTL 校准性能模型

本文记录把性能模型对齐到真实 RTL 的第一轮工作：环境、测量方法、从 RTL 上直接测得的微架构事实、模型为此做的修正，以及当前的误差与残余差异。

## 1. 参照物

| 项 | 取值 |
|---|---|
| RTL | Chipyard 1.11.0 `HwachaRocketConfig`（`generators/hwacha` 为最后一个含 Hwacha 的 Chipyard 版本），叠加 `~/hwacha-compiler/patches/chipyard-hwacha-rtl-fixes.patch`（VI$ 行宽、前端取指、SMU TLB 特权级、ALL 归约四处修复，否则任何 vf 块都跑不通） |
| Hwacha 配置 | `DefaultHwachaConfig`：1 lane、4 bank × 256 行、8 个序列器槽、无混合精度；顶层 `confvru = false`，即没有 VRU |
| 存储系统 | sbus 128 位；SiFive InclusiveCache 512 KB、8 路、单 bank（子 bank 因子 4）；SimDRAM `mm_magic` 模型（近似零延迟主存，mbus 500 MHz，核心与总线时钟由 harness 生成） |
| 仿真器 | Verilator 5.020，`-O1`，实测约 20 k 周期/秒 |
| 软件 | esp-tools GCC 9.2（`-march=rv64gcxhwacha`）+ esp-tests 裸机运行时；Spike + hwacha 扩展做功能验证 |
| 运行方式 | 裸机 M 态，无地址翻译；`rdcycle` 包住整个 stripmine 循环（含结尾 `fence`），与 hwacha-compiler 的测法一致 |

由于基准程序先用标量核初始化数组、再调用向量内核，数据在内核开始时已经驻留 L2（最后写入的约 16 KB 还脏在 Rocket 的 L1D 中）。因此这些测量刻画的是**向量单元 + L2 命中路径**，而不是 DRAM。

## 2. 测量程序（`rtl/`）

- `rtl/main.c` + `rtl/kernels.S`：把 `kernels/*.S` 原样 `#include` 进来（`# @` 指令对汇编器只是注释，另加 `.align 3`），控制线程用内联汇编实现，每个内核计时两次：第一次含首次触碰效应（L1D 脏行探测、VI$ 冷缺失），第二次（`_warm`）为稳态。除 `dgemm_opt`、`fma_peak` 外全部与标量参考比对。
- `rtl/micro.c` + `kernels/micro_*.S`：隔离单一通路的微基准：空块、纯 load、纯 store、load+store（有/无数据依赖）、两条独立 load、bank 本地 ALU 链、FMA 依赖链。
- `rtl/probe.c`、`rtl/probe2.c`：用 `rdcycle` 夹住单条 `vsetvl`/`vf`/`fence`/`vsetcfg`/`vmcs`，直接读出固定开销与延迟。
- `scripts/compare_rtl.py`：解析 `rtl/results/*.log` 的 `RESULT` 行，用 `configs/rtl-hwacha-rocket.json` 运行两个模型，输出误差表（以 `_warm` 为准）。

```bash
cd rtl && make N=4096 && make spike && make rtl          # 全部内核：Spike 验证后在 RTL 计时（约 30 分钟，标量参考占大头）
make micro-rtl && make probe-rtl && make probe2-rtl       # 微基准与探针（各 1–3 分钟）
python3 scripts/compare_rtl.py                            # 与模型比较
```

为了能在 Spike 与 RTL 上运行，内核文件做了两处与 ISA 实现相关的修改：`vsetcfg` 至少声明 1 个谓词寄存器并在块首 `vpset vp0`（未加谓词的指令隐式引用 vp0，`nvp=0` 时 Spike 报非法指令），以及使用汇编器接受的谓词/比较语法（`@!vp0`、`vcmpeq vp0, vs0, vv0`）。

## 3. 从 RTL 直接测得的事实

探针（`rtl/results/probe.log`、`probe2.log`）：

| 项目 | 周期 | 说明 |
|---|---|---|
| `vsetvl` / `vf` / `vsetcfg` / `vmcs` 发射 | 2 / 2 / 2–3 / 2–3 | 控制线程侧每条 RoCC 命令约 2 拍 |
| 空闲时 `fence` | 2 | |
| 空块（仅 `vpset`）vl=8 后的 `fence` | 9 | 一个 strip 的谓词写 + 退休 + fence 应答 |
| 空块 vl=2048 后的 `fence` | 264 | 256 个 strip × 1 拍：PLU 操作每拍一个 strip |
| 单 strip load 后的 `fence` | 35 | VMU 发射 → L2 命中 → 写回 VRF 的往返延迟 |
| vl=2048 load 后的 `fence` | 1061 | 1024 个 beat 每拍一个 + 约 37 拍尾延迟 |
| 连续 3 个 load 块再 `fence` | 3118 | 3072 beat + 46：块与块之间无间隙 |
| 稳态整循环：空块 / load / store / copy（N=4096，2 块） | 538 / 2118–2140 / 2149–2365 / 4471–4675 | 见微基准表 |
| 标量核刚写过 32 KB 后再向量 load / store | +1050 / +800 | L1D 脏行被 L2 探测并写回，约 4 拍/行（`probe3.log`） |
| 标量核刚读过 32 KB 后再向量 load / store | +515 / +270 | L1D 中的共享副本仍需探测（load 约 2 拍/行，store 约 1 拍/行） |
| 首次运行某内核 | +70–330 | VI$ 冷缺失 + 上述探测 |

由此得到的微架构结论：向量 load 与 store 各自都能达到每拍一个 16 字节 beat；PLU/ALU 类操作每拍一个 strip；FMA 依赖链在长向量下完全被 chaining 掩盖延迟（8 条相关 FMA × 512 strip = 8192 拍，实测 8223）；两条 load 在 VMU 中完全流水（4193 ≈ 4096 + 97）；load 与 store 混合时总吞吐降到约每 1.1 拍一个 beat（4498–4675 对 4096 beat）。

## 4. 据此对模型做的修正

| 修正 | 依据 |
|---|---|
| `configs/rtl-hwacha-rocket.json`：1 lane、无 VRU、512 KB 单 bank L2、`warm_l2`、TLB 缺失代价 0 | 对应 RTL 配置与裸机 M 态 |
| 两个模型加入 `warm_l2`：按 va 引用范围（`n + offset + 16` 个元素）预热数组，仅被索引访存引用的数组整体预热 | 基准中数据由标量核初始化后驻留 L2 |
| 序列器加入第二调度端口：主端口任意操作，第二端口只发 VSU/VGU/VQU 类操作 | 纯 store 微基准：原模型让 `vpset` 的 256 个 strip 独占发射端口，store 数据读出被推迟约 230 拍；RTL 源码中 `vsu_consider`/`vgu_consider`/`vqu_consider` 走独立端口 |
| 加入 `store_beat_cycles` / `load_beat_cycles`（可为分数的端口占用）与 `vf_block_overhead` 校准旋钮 | 供后续拟合混合访存与块开销 |
| 内核文件：`vp=1` + `vpset vp0`；`csaxpy` 改为汇编器语法 | Spike/RTL 约束 |

## 5. 当前误差

"稳态"的定义很重要：基准程序在两次计时之间用标量核做结果验证，验证循环把结果数组的最后 16 KB 拉回 Rocket 的 L1D，随后的向量 load/store 又要探测这些行。因此 `rtl/main.c` 每个内核计时三次：冷（含初始化写入留下的脏行与 VI$ 冷缺失）、`_warm`（前面有验证循环）、`_warm2`（紧接 `_warm`，无标量访问干扰）。模型对应 `_warm2`。

微基准（N=4096，`rtl/results/micro-n4096.log`，无验证循环，两次计时即稳态）：

| kernel | RTL 冷 | RTL 稳态 | C++ 模型 | 误差 |
|---|---|---|---|---|
| micro_empty | 617 | 549 | 521 | −5.1% |
| micro_load | 2439 | 2140 | 2097 | −2.0% |
| micro_store | 2935 | 2149 | 2092 | −2.7% |
| micro_load2 | 4229 | 4207 | 4154 | −1.3% |
| micro_ldst（无依赖 load+store） | 4496 | 4498 | 4149 | −7.8% |
| micro_copy（有依赖 load+store） | 4330 | 4675 | 4149 | −11.3% |
| micro_alu | 8227 | 8227 | 8331 | +1.3% |
| micro_fma_dep | 8227 | 8254 | 8334 | +1.0% |

Python 模型与 C++ 模型在这些内核上相差不到 0.1%。计算侧（ALU、FMA、chaining、发射节奏）误差在 1%–1.5%；纯 load 误差 1%–2%；**store 参与的访存**是最大的残余差异。三次构建（只是增加了别的内核、改变了数据与代码布局）中同一 `micro_store` 稳态分别为 2365、2149、2456，`micro_copy` 稳态为 4518、4675、4604，说明 RTL 的 store 吞吐对地址/布局敏感，在每 beat 1.0–1.2 拍之间波动；纯 load 三次都是 2118–2149。进一步的变体（同一次构建）：无依赖的 load+store 4602、store 在前 4637、同一数组原地 copy 4477，与有依赖的 copy 4604 接近，说明混合开销既不来自数据依赖，也不来自访问顺序，更像 L2（InclusiveCache）处理 Put 的路径或 L1D 探测（数据由标量核写入后仍可能处于 L1D）造成。模型对 store 按每 beat 1.0 拍计，`store_beat_cycles` 旋钮可按需要设为 1.1–1.2。

完整内核（`rtl/results/rtl-n4096.log`）的比较表由 `scripts/compare_rtl.py` 生成，见第 7 节。

## 6. 尚未建模的 RTL 效应

- 标量核写入后仍脏在 L1D 的数据被向量单元首次访问时的探测代价（约 3.6 拍/行）以及 VI$ 冷缺失：属于"冷启动"，模型对应稳态。
- load/store 混合时的额外开销（见上）。
- 每个 vf 块约 +15 拍的固定开销（空块 549 对 521）。
- 控制线程：模型按每条命令 1 拍 + 固定簿记周期计，RTL 为每条 RoCC 命令约 2 拍；对长向量块影响 < 1%。

## 7. 完整内核对比

`python3 scripts/compare_rtl.py --logs rtl/results/rtl-n4096.log`（N = 4096，以 `_warm2` 为准）：

| kernel | RTL 冷 | RTL `_warm` | RTL `_warm2` | C++ 模型 | 误差 | Python 模型 | 误差 |
|---|---|---|---|---|---|---|---|
| fma_peak | 12311 | 12342 | 12309 | 12344 | +0.3% | 12343 | +0.3% |
| saxpy | 3930 | 3683 | 3283 | 3241 | −1.3% | 3240 | −1.3% |
| dgemm_opt | 8534 | 8404 | 8403 | 8253 | −1.8% | 8252 | −1.8% |
| gather | 9903 | 9069 | 8551 | 8357 | −2.3% | 8353 | −2.3% |
| csaxpy | 4659 | 4327 | 3887 | 3755 | −3.4% | 3754 | −3.4% |
| sfilter | 5884 | 5588 | 5306 | 5580 | +5.2% | 5526 | +4.1% |
| vvadd | 7519 | 7186 | 6646 | 6197 | −6.8% | 6196 | −6.8% |
| daxpy | 7457 | 7152 | 6752 | 6197 | −8.2% | 6196 | −8.2% |

平均绝对误差 3.6%，最大 8.2%（C++）。`_warm` 与 `_warm2` 之间 400–540 拍的差别（saxpy、csaxpy、vvadd、daxpy、sfilter、gather）就是验证循环留在 L1D 中的共享副本造成的探测代价，与第 3 节探针测得的每行 1–2 拍一致；这部分属于基准程序的副作用，不是向量单元的性能，模型不必再现。

残余的系统性偏差集中在**双精度的 2 load + 1 store 流式内核**（vvadd、daxpy −7%～−8%）：每块 2048 个 load beat 与 1024 个 store beat，RTL 每块约 3320 拍，模型约 3100 拍，多出的约 220 拍相当于混合流中每个 store beat 多花约 0.2 拍，与微基准 copy/ldst 的结论一致；单精度版本（saxpy、csaxpy）每块 beat 数减半而固定开销相同，偏差就小得多。sfilter 模型偏慢 5%：三个偏移 1 个元素的 load 在模型里各自按对齐边界多算了 beat，而 RTL 的合并更好。

### N = 16384 的结果（`rtl/results/rtl-n16384.log`，只有冷启动计时，运行在 fma_peak 前被中止）

| kernel | RTL 冷 | C++ 模型 | 误差 |
|---|---|---|---|
| dgemm_opt | 33096 | 32832 | −0.8% |
| sfilter | 22766 | 22073 | −3.0% |
| csaxpy | 17416 | 14906 | −14.4% |
| saxpy | 15539 | 12853 | −17.3% |
| daxpy | 31651 | 24677 | −22.0% |
| gather | 43016 | 33083 | −23.1% |
| vvadd | 32912 | 24677 | −25.0% |

这一组不能作为校准依据：基准程序的静态数组总量约 1.1 MB，超过 512 KB 的 L2，数据不再驻留，而 `rtl-hwacha-rocket.json` 假定 `warm_l2`，且 RTL 一侧的主存路径（SimDRAM 经 500 MHz、64 位的 mbus）尚未测量和建模。要在这个规模上比较，需要先用超过 L2 容量的流式微基准测出 RTL 的主存带宽与延迟，把它写进配置，再关闭 `warm_l2`。

## 8. 第二轮：执行驱动、块内标量与分支、2 lane、主存路径

### 8.1 执行驱动（踪迹模式）

给 `esp-isa-sim` 的 Hwacha 扩展打了补丁（`~/hwacha-compiler/esp-isa-sim/hwacha`：`insns/vf.h`、`decode_hwacha_ut.h`、`insns_ut/{vamo*,vlsegx*,vssegx*}.h`、`hwacha.{h,cc}`）：设置环境变量 `HWACHA_TRACE=<file>` 时，每条工作线程指令输出一行
`H: WT pc=… inst=… next=… vl=… act=<每元素活跃掩码>`，每个访存元素输出一行 `HMEM: read/write/rmw <addr> ut=<元素>`。`scripts/hwacha_trace.py run` 生成踪迹，`range` 用 ELF 符号表给出某个 vf 块的地址范围。两个模型都接受 `--trace <file> [--trace-range lo:hi] [--trace-blocks a:b]`：每次 vf 从踪迹取动态指令序列（分支结果、循环次数）、每条指令的活跃掩码（谓词化访存只为活跃元素发 beat，变延迟单元按活跃元素计）与访存地址（索引/原子访存直接用踪迹地址），控制线程按踪迹中每块的 vl 做 stripmine，`warm_l2` 预热踪迹触及的行。

这样就可以直接校验 hwacha-cc 编译出的 OpenCL 内核（`kernels/hcc/*.S` 原样复制自 `hwacha-cc/test/bench/bench.s`，含分歧循环、一致性分支、块内标量访存与乘法、谓词逻辑）。RTL 侧用 `rtl/hcc/`（三次计时版的 bench_main.c）取稳态：

| hwacha-cc 内核（N = 1024） | RTL 冷 | RTL 稳态 | 模型（踪迹驱动） | 误差 |
|---|---|---|---|---|
| saxpy | 1640 | 875 | 842 | −3.8% |
| clamp_scale（select → 两条互斥谓词化写） | 1049 | 734 | 685 | −6.7% |
| stencil（3 点） | 1822 | 1441 | 1361 | −5.6% |
| gather（随机置换） | 2238 | 1688 | 1637 | −3.0% |
| divloop（每元素 0–15 次的分歧循环，17 次一致性分支，136 个向量操作） | 17278 | 16711 | 14689 | −12.1% |

静态模式（注解给分支次数、伪随机索引）对这些内核无法给出有意义的结果；踪迹模式下 Python 与 C++ 模型逐周期一致。

### 8.2 块内标量指令与一致性分支（`rtl/results/probe5.log`）

| 项目 | RTL（稳态） | 结论与模型参数 |
|---|---|---|
| 4 条相关标量 load（SMU） | 每条约 9.5 拍 | `scalar_smu_latency` 30 → 12 |
| 4 条相关标量乘 | 每条约 4 拍 | `scalar_muldiv_latency` 8 → 5 |
| 8 条相关标量加 | 每条 1 拍 | 与模型一致 |
| 一致性分支，vl = 8 | 约 7 拍 | `branch_resolve_latency` 4 → 0 |
| 一致性分支，vl = 1024 | 768 拍 = 128 strip × 6 | 谓词归约每 strip 6 拍：新增 `branch_strip_cycles`（默认 4，RTL 配置 6） |

分支的每 strip 6 拍解释了 hwacha-cc 工程日志里"vcjal 约 50 拍"的观察（vl = 64 时 8 个 strip），也是 divloop 从 −26% 收敛到 −12% 的主要修正；残余 12% 尚未定位。

### 8.3 主存路径（`rtl/results/probe4.log`）

用 2 MB 标量写把 L2 冲掉后再向量 load 32 KB：2103 拍，与 L2 命中时（2137）相同；单个 strip 从"主存"读 44–69 拍，命中时 35 拍。即 Chipyard 1.11 默认 harness 的 SimDRAM（`mm_magic`）几乎没有延迟和带宽限制，L2 缺失只多 10–15 拍。`rtl-hwacha-rocket*.json` 据此把 DRAM 时序设为极快；`warm_l2` 对结果影响很小。之前 N = 16384 的偏差因此确认全部来自冷启动效应而不是 DRAM。

### 8.4 2 lane RTL

新增 Chipyard 配置 `HwachaL2RocketConfig = WithNLanes(2) ++ HwachaRocketConfig`（`generators/chipyard/src/main/scala/config/HwachaLaneConfigs.scala`），Verilator 构建约 1 小时。微基准（N = 4096，稳态）：

| kernel | 1 lane RTL | 2 lane RTL | 2 lane 模型 | 误差 |
|---|---|---|---|---|
| micro_alu | 8224 | 4129 | 4235 | +2.6% |
| micro_fma_dep | 8251 | 4155 | 4238 | +2.0% |
| micro_empty | 549 | 290 | 265 | −8.6% |
| micro_load | 2149 | 2662 | 2701 | +1.5% |
| micro_load2 | 4213 | 5244 | 5362 | +2.3% |
| micro_store | 2456 | 3206 | 2696 | −15.9% |
| micro_copy | 4604 | 5735 | 5357 | −6.6% |
| micro_ldst | 4602 | 5761 | 5357 | −7.0% |

全部内核在 2 lane RTL 上的运行在 gather 处被 RTL 自己的断言终止（`vmu.scala:265`，`IBox: qcntr too large. aret broken`，多 lane 的 `IBoxML` 处理索引访存时触发），这证实了 hwacha-compiler 工程日志里"多 lane 配置未验证"的说法：开源 RTL 的 2 lane 只能跑单位/常量步长访存。gather 之前的 5 个内核（稳态，`rtl/results/rtl-n4096-l2.log`）：

| kernel | 1 lane RTL | 2 lane RTL | 2 lane 模型 | 误差 |
|---|---|---|---|---|
| vvadd | 6646 | 8322 | 8020 | −3.6% |
| daxpy | 6752 | 8290 | 8020 | −3.3% |
| csaxpy | 3887 | 5329 | 4694 | −11.9% |
| saxpy | 3283 | 5020 | 4029 | −19.7% |
| sfilter | 5306 | 5798 | 6727 | +16.0% |

双精度流式内核 2 lane 慢 25%，模型跟得上；单精度内核（saxpy、csaxpy）2 lane 慢 37%–53%，模型只解释了一半。单精度时每个 lane 的单位步长段只有 32 字节（8 个元素），两个 lane 交替写同一个 64 字节行的两半，可能是 InclusiveCache 对同一行交错部分写的额外代价。dgemm_opt 与 fma_peak 的 2 lane 数据用 `make rtl-nogather`（跳过 gather）单独获取。

计算侧随 lane 数线性扩展（8224 → 4129），模型一致。**访存侧 2 lane 反而比 1 lane 慢**（load 2149 → 2662，store 2456 → 3206）：Chipyard 集成里所有 lane 的 VMU 经 `TLWidthWidget(16)` 汇入 RoCC 的同一个 TileLink 节点，再经 tile 的主交叉开关进入 sbus，两个 lane 争用一个 128 位端口，仲裁还带来约每 beat 0.3 拍的额外开销；论文中每 lane 有独立的 L2 端口。模型新增 `rocc_shared_port`（所有 lane 与 VRU 汇入一个端口）与 `rocc_switch_penalty`（源切换的分数周期，按信用折算），RTL 配置取 true / 0.3。这意味着在开源集成上，多 lane 只对计算受限内核有意义。

### 8.5 当前误差汇总（C++ 模型，`store_beat_cycles` 1.15）

单 lane 8 个内核 + 10 个微基准：平均 2.5%，最大 8.0%（sfilter）。2 lane 微基准：平均 6.0%，最大 15.9%（store）；2 lane 流式内核：双精度 ±4%，单精度 −12%～−20%。hwacha-cc 内核（踪迹驱动）：4 个在 3%–7%，divloop −12%。

## 9. 下一步

1. store 的代价随布局在每 beat 1.0–1.2 拍之间波动、2 lane 下更高：需要 TileLink 通道级跟踪定位是 InclusiveCache 的 Put 路径还是 tile 交叉开关的仲裁。
2. divloop 残余 −12%：块内谓词逻辑（vpop）与谓词化向量操作交错时的调度细节。
3. sfilter +8%：3 个 load 占 9 个序列器槽位时的窗口行为，模型比 RTL 保守。
4. 冷启动效应（L1D 脏行/共享行探测）可作为 `warm_l2` 的可选描述建模。
5. VRU 与混合精度在开源 RTL 上仍没有可用配置，只能对论文数据做趋势校验。
