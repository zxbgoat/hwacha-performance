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

双精度流式内核 2 lane 慢 25%，模型跟得上；单精度内核（saxpy、csaxpy）2 lane 慢 37%–53%，模型只解释了一半。单精度时每个 lane 的单位步长段只有 32 字节（8 个元素），两个 lane 交替写同一个 64 字节行的两半，可能是 InclusiveCache 对同一行交错部分写的额外代价。

跳过 gather 的第二次运行（`make rtl-nogather`，`rtl/results/rtl-nogather-n4096-l2.log`，数据布局随之改变）补上了计算受限内核，也暴露了布局敏感性：

| kernel | 1 lane RTL | 2 lane RTL（第二次运行） | 2 lane 模型 | 误差 |
|---|---|---|---|---|
| fma_peak | 12309 | 6164 | 6200 | +0.6% |
| dgemm_opt | 8403 | 4784 | 5712 | +19.4% |
| csaxpy | 3887 | 3929（第一次运行 5329） | 4694 | +19.5% |
| sfilter | 5306 | 5423（第一次运行 5798） | 6727 | +24.0% |

fma_peak 精确地减半，模型一致。dgemm_opt 在 RTL 上达到 1.76× 扩展：它的两条 B 行 load 加起来每周期约 1.7 个 beat，**超过了一个 128 位端口的 1 beat/周期**，说明 2 lane 的访存瓶颈并不是 RoCC 端口本身，`rocc_shared_port` 的建模对 load 为主的内核过于悲观。同一个 csaxpy 在两次 2 lane 运行中相差 36%（5329 对 3929），只因为二进制里数组的位置不同，这与单 lane 下 store 吞吐随布局波动是同一类现象，且在 2 lane 下被放大。要真正解释它需要 TileLink 通道级或 InclusiveCache 内部的跟踪，目前只能把 2 lane 的访存侧标为"未校准"。

计算侧随 lane 数线性扩展（8224 → 4129），模型一致。**访存侧 2 lane 反而比 1 lane 慢**（load 2149 → 2662，store 2456 → 3206）：Chipyard 集成里所有 lane 的 VMU 经 `TLWidthWidget(16)` 汇入 RoCC 的同一个 TileLink 节点，再经 tile 的主交叉开关进入 sbus，两个 lane 争用一个 128 位端口，仲裁还带来约每 beat 0.3 拍的额外开销；论文中每 lane 有独立的 L2 端口。模型新增 `rocc_shared_port`（所有 lane 与 VRU 汇入一个端口）与 `rocc_switch_penalty`（源切换的分数周期，按信用折算），RTL 配置取 true / 0.3。这意味着在开源集成上，多 lane 只对计算受限内核有明确收益；访存侧的行为随数据布局变化很大，尚未找到确定的机理。

### 8.5 当前误差汇总（C++ 模型，`store_beat_cycles` 1.15）

单 lane 8 个内核 + 10 个微基准：平均 2.5%，最大 8.0%（sfilter）。2 lane 微基准：平均 6.0%，最大 15.9%（store）；2 lane 流式内核：双精度 ±4%，单精度 −12%～−20%。hwacha-cc 内核（踪迹驱动）：4 个在 3%–7%，divloop −12%。

## 10. 第三轮：TileLink 通道级跟踪、数组对齐、IBoxML 修复、Rodinia、除法/开方与跨步访存

### 10.1 TileLink 通道级跟踪（`+verbose +hwacha_tl_trace=1`）

在 Chisel 里加了 PlusArg 门控的 printf：`hwacha/vmu-memif.scala` 的 `VMUTileLink` 打印每次 A/D 通道握手（周期、opcode、地址、source、握手前等待拍数），`rocket-chip-inclusive-cache/.../Scheduler.scala` 打印 L2 bank 内侧 A/D 与外侧 A/C/D 握手以及当时有效的 MSHR 数。Chisel printf 只在 `+verbose` 下输出，而 +verbose 同时打开 Rocket 的提交日志，仿真慢 30–50 倍，只能跑微基准。`scripts/tl_trace_stats.py` 按 RESULT 行切段统计（`rtl/results/tlv-micro-n4096.log`、`tlv-micro-n4096-l2.log`）。

单 lane 微基准（N = 4096，稳态）在 VMU 的 A 通道上看到的：

| kernel | A 请求数 | beats/拍 | A 等待均值 | 被阻塞的请求 | L2 MSHR 均值 |
|---|---|---|---|---|---|
| micro_load | 2048 Get | 0.993 | 0.01 | 0.7% | 1.2 |
| micro_store | 2048 PutPartial | 0.941 | 0.06 | 3.6% | 5.3 |
| micro_copy | 2048 Get + 2048 Put | 0.993 | 0.01 | 0.7% | 1.2 |
| micro_load2 | 4096 Get | 0.993 | 0.01 | 0.7% | 1.2 |
| micro_ldst | 2048 + 2048 | 0.923 | 0.08 | 3.3% | 7.1 |
| micro_stld | 2048 + 2048 | 0.967 | 0.03 | 1.8% | 5.5 |

L2 内侧 A→D 延迟：load 命中 4 拍；store（PutPartial）在纯 store 流里平均 15 拍（一行的第一拍 7 拍、后三拍 18 拍），在混合流里 22 拍。机制（`Scheduler.scala`）：InclusiveCache 每拍只接受一个请求；每个请求（命中也一样）要分配一个 MSHR，同一 set 已有 MSHR 时排到它后面等它 reload 后串行服务；PutPartial 要读-改-写数据阵列，占 MSHR 约 5 拍，Get 约 1 拍。所以 store 流会把 MSHR 用到 9–10 个（上限）并偶尔反压 A 通道，load 流几乎不会。这就是 store 每 beat 略多于 1 拍（模型里 `store_beat_cycles` 1.15）以及 store/load 混合略慢的来源；同 set 排队意味着相距 64 KB 整数倍的两条流会互相串行，是布局敏感性的另一个来源。

2 lane（`tlv-micro-n4096-l2.log`）：两条 lane 按 64 B 交错访问（lane 0 取偶数行、lane 1 取奇数行），A 通道严格轮流、每拍恰好一个请求进入 L2（没有任何一拍出现两个握手），每个请求平均等 1.0 拍。也就是说 Chipyard 集成下所有 lane 共用 RoCC 的一条 128 位 TileLink 路径和单个 L2 bank，2 lane 的访存吞吐与 1 lane 完全相同（RTL：micro_load 2121 拍、micro_copy 4246 拍，与 1 lane 一致）。模型里 `l2_banks = 1`（每 bank 每拍一个请求）已经表达了这个限制，此前为拟合未对齐数据加的 `rocc_shared_port` / `rocc_switch_penalty` 近似已在两个 RTL 配置里关闭。

### 10.2 数组 4 KB 对齐

`rtl/main.c`、`rtl/micro.c` 的数组改为 `__attribute__((aligned(4096)))` 之后，2 lane 微基准与模型（关闭共享端口近似）的误差：

| kernel | 2 lane RTL 稳态 | 模型 | 误差 |
|---|---|---|---|
| micro_load | 2121 | 2090 | −1.5% |
| micro_store | 2232 | 2085 | −6.6% |
| micro_copy | 4246 | 4134 | −2.6% |
| micro_load2 | 4181 | 4139 | −1.0% |
| micro_ldst / stld / inplace | 4188 / 4192 / 4187 | 4134 / 4139 / 4133 | −1.3% |
| micro_alu / fma_dep | 4155 / 4131 | 4235 / 4238 | +1.9% / +2.6% |

平均 3.7%（不含 micro_empty 的 −17%，它只有 319 拍）。之前 2 lane 微基准 15.9% 的 store 误差来自未对齐布局。

### 10.3 IBoxML 断言（2 lane 索引访存）

`vmu.scala` 的 `IBoxML` 用一个 2 位的 `qcntr` 记录 abox2 队列里本 op 还剩几个子操作、以此决定何时发 `aret`（通知 MRT 释放地址寄存器）；索引访存每拍入队一个子操作，计数溢出触发 `assert(qcntr <= 1)`。改为给每个子操作打 `last` 标记（`VMUDecodedOp.last`），abox2 取走带 last 的子操作时发 `aret`，去掉 `qcntr`/`aret_pending`。修复后的 2 lane 仿真器能跑完含 gather 的完整基准（结果见 10.6）。

### 10.4 多入口 vf 块的踪迹映射与 Rodinia 内核

hwacha-cc 为一个 OpenCL 内核生成多个入口（`*_wt`、`*_wt_r0_b*`、`*_wt_a0`），控制线程按块调用。两个模型的 `--trace-base <pc>`（默认取 `--trace-range` 的下界）把踪迹 pc 减去内核文件第一条指令的地址得到指令序号，于是一个内核文件可以按原地址顺序连续放下全部入口块。`scripts/extract_hcc_kernel.py` 从 hwacha-cc 的 `.s` 抠出整个 `_wt` 区域并生成注解（寄存器配置取自 spike-hlog 的 `H: VSETCFG` 行）。`rtl/rodinia/` 是 `hwacha-cc/test/apps` 四个程序的三次计时版；`scripts/compare_rodinia.py` 取踪迹里第二次运行的 vf 块与 RTL 的 `_warm2` 比较，并接入 ctest（`calibrate-rodinia`）。

### 10.5 Rodinia 内核（踪迹驱动）

`rtl/rodinia/`（N = 1024 点 / 2048 记录，`rtl/results/rodinia-*.log`）三次计时，取 warm2；模型取踪迹中第二次运行的 vf 块：

| 内核（hwacha-cc 编译的 OpenCL） | vf 块数 | RTL 冷 | RTL 稳态 | C++ | 误差 | Python | 误差 |
|---|---|---|---|---|---|---|---|
| nn（跨步 load ×2、`vfsqrt.s`、一致性分支） | 4 | 15106 | 14599 | 13260 | −9.2% | 13227 | −9.4% |
| kmeans_swap（索引 load + 索引 store，谓词化） | 40 | 23593 | 21645 | 20910 | −3.4% | 17603 | −18.7% |
| kmeans_c（7 个入口块、索引 load、标量移位链） | 248 | 64650 | 63993 | 64890 | +1.4% | 67112 | +4.9% |
| pgain（块内标量 load/store、分歧循环、索引访存、跨步 load/store） | 40 | 26048 | 25272 | 25516 | +1.0% | 25475 | +0.8% |
| pathfinder（150 条指令的块、10 个分支、索引访存） | 36 | 145084 | 144765 | 136179 | −5.9% | 134900 | −6.8% |

C++ 模型平均 |误差| 4.2%，最大 9.2%（nn）（表中 C++ 列为 10.9 节 L2 store 通路模型之后的数值）。这是在校准了除法/开方吞吐（10.7）和跨步访存每元素一个请求（10.7）之后的结果；校准前 nn 是 +228%。Python 模型在 kmeans_swap 上 −18.7%：索引 store 的在途请求数（VMT）计账与 C++ 不同，C++ 显示 16% 的时间 VMT 满而 Python 没有；C++ 与 RTL 更接近，Python 保留为已知偏差。

### 10.6 对齐后的完整基准与 2 lane 含 gather 的基准

单 lane，数组 4 KB 对齐（`rtl/results/rtl-n4096-aligned.log`），`store_beat_cycles` 1.10：

| kernel | RTL 冷 | RTL 稳态 | C++ | 误差 | Python | 误差 |
|---|---|---|---|---|---|---|
| vvadd | 7678 | 6552 | 6401 | −2.3% | 6400 | −2.3% |
| saxpy | 4262 | 3255 | 3342 | +2.7% | 3342 | +2.7% |
| daxpy | 7327 | 6505 | 6401 | −1.6% | 6400 | −1.6% |
| csaxpy | 4985 | 3859 | 3856 | −0.1% | 3856 | −0.1% |
| sfilter | 5798 | 5264 | 5682 | +7.9% | 5650 | +7.3% |
| gather | 10087 | 8543 | 8562 | +0.2% | 8557 | +0.2% |
| dgemm_opt | 8538 | 8402 | 8253 | −1.8% | 8252 | −1.8% |
| fma_peak | 12311 | 12309 | 12344 | +0.3% | 12343 | +0.3% |

C++ 平均 |误差| 2.1%，最大 7.9%（sfilter，与第一轮相同：序列器窗口，见 `25-design-space.md` 第 1 节）。`store_beat_cycles` 取 1.15 时平均 2.3%、最大 8.9%；对未对齐的旧日志取 1.10 时平均 2.6%——1.10 在两种布局下都合适。

2 lane，修复 IBoxML 后含 gather 的完整基准（`rtl/results/rtl-n4096-l2-fixed.log`，数组对齐，`rtl-hwacha-rocket-l2.json`：关闭共享端口近似、`store_beat_cycles` 1.10）：

| kernel | 1 lane RTL 稳态 | 2 lane RTL 冷 | 2 lane RTL 稳态 | C++ | 误差 | Python | 误差 |
|---|---|---|---|---|---|---|---|
| vvadd | 6552 | 7466 | 6260 | 6182 | −1.2% | 6181 | −1.3% |
| saxpy | 3255 | 4229 | 3335 | 3110 | −6.7% | 3109 | −6.8% |
| daxpy | 6505 | 7429 | 6645 | 6182 | −7.0% | 6181 | −7.0% |
| csaxpy | 3859 | 4828 | 3934 | 3623 | −7.9% | 3622 | −7.9% |
| sfilter | 5264 | 5937 | 5429 | 5204 | −4.1% | 5196 | −4.3% |
| gather | 8543 | 10034 | 8766 | 8283 | −5.5% | 8287 | −5.5% |
| dgemm_opt | 8402 | 5277 | 4776 | 4552 | −4.7% | 4529 | −5.2% |
| fma_peak | 12309 | 6167 | 6165 | 6200 | +0.6% | 6199 | +0.6% |

C++ 平均 |误差| 4.7%，最大 7.9%（csaxpy）。所有访存内核 2 lane 都不比 1 lane 快（daxpy、gather 甚至更慢），只有 dgemm 与 fma_peak 加速约 2×——与 10.1 的机制一致。模型对 2 lane 的访存内核一致地偏乐观 1%–8%：两条 lane 在单一路径上严格轮流时的切换代价没有建模；RTL 本身也有布局/替换历史带来的波动（vvadd 与 daxpy 的访存模式相同，RTL 却相差 6%，而 1 lane 时两者相同）。gather 的 2 lane 结果证明 10.3 的 IBoxML 修复可用（此前该配置在 `vmu.scala:265` 断言退出）；dgemm 第二轮的 +19% 误差（未对齐布局 + 共享端口近似）现在是 −4.7%。

### 10.7 除法/开方、跨步访存与 VSDQ（`rtl/results/micro-n4096-aligned2.log`）

新增微基准 `micro_fsqrt_s`、`micro_fdiv_s`、`micro_fdiv_d`（操作数先用 vs1 填成正常数：未初始化的 VRF 是 0，hardfloat 对 0/NaN 走特殊值快速路径，那样测出来每元素只有 0.8 拍）、`micro_lstride`、`micro_sstride`（32 位元素、步长 8 B，即 Rodinia nn 的结构体访问）：

| 项目 | RTL（每 lane、稳态） | 模型修正 |
|---|---|---|
| `vfdiv.s` / `vfdiv.d` | 12364 / 12362 拍 = 每元素 3.02 拍 | `fdiv_cycles_per_elem` 22 → 3（两个 `DivSqrtRecF64` slice 各约 6 拍） |
| `vfsqrt.s` | 20559 拍 = 每元素 5.02 拍 | 新参数 `fsqrt_cycles_per_elem` = 5 |
| 跨步 load（4 B、步长 8 B） | 4172 拍 = 每元素 1.02 拍 | 跨步/索引访存每个元素一个 TileLink 请求（之前把落在同一 16 B beat 的元素合并成一个请求，少算一半） |
| 跨步 store | 4207 拍 = 每元素 1.03 拍 | 同上；`store_beat_cycles` 只作用于单位步长的 16 B beat；C++ 的 VSDQ 改为按 16 B 数据量而不是按请求数计条目（否则跨步 store 每 strip 8 个请求把 8 项的 VSDQ 占满，慢 11%） |

修正后单 lane 微基准（数组 4 KB 对齐）：

| kernel | RTL 冷 | RTL 稳态 | C++ | 误差 |
|---|---|---|---|---|
| micro_load / load2 | 2649 / 4180 | 2144 / 4180 | 2097 / 4154 | −2.2% / −0.6% |
| micro_store | 3002 | 2117 | 2399 | +13.3%（见 10.8） |
| micro_copy / ldst / stld / inplace | 4392 / 4602 / 4263 / 4643 | 4474 / 4345 / 4184 / 4521 | 4456 / 4456 / 4461 / 4447 | −0.4% / +2.6% / +6.6% / −1.6% |
| micro_lstride / sstride | 4198 / 4205 | 4172 / 4207 | 4146 / 4141 | −0.6% / −1.6% |
| micro_fdiv_s / fdiv_d / fsqrt_s | 12365 / 12385 / 20597 | 12364 / 12362 / 20559 | 12436 / 12438 / 20628 | +0.6% / +0.6% / +0.3% |
| micro_alu / fma_dep | 8227 / 8249 | 8223 / 8248 | 8331 / 8334 | +1.3% / +1.0% |
| micro_empty | 616 | 570 | 521 | −8.6% |

平均 |误差| 2.8%。

### 10.8 store 吞吐随布局波动的来源（`rtl/results/probe6.log`）

探针 6 在一块 64 KB 对齐的 512 KB 缓冲区里改变 store 流的起点偏移（相对 64 KB 边界，即 L2 set 周期）和 copy 的源/目的距离：

| store 起点偏移 | 0 | 64 B | 1 KB | 4 KB | 8 KB | 16 KB | 32 KB | 48 KB |
|---|---|---|---|---|---|---|---|---|
| 稳态周期（2048 beat） | 2137 | 2368 | 2110 | 2347 | 2347 | 2110 | 2110 | 2247 |
| 每 beat 拍数 | 1.04 | 1.16 | 1.03 | 1.15 | 1.15 | 1.03 | 1.03 | 1.10 |

load 在所有偏移下都是 2109 拍（每 beat 1.03 拍）。copy 的目的与源相距 36 KB / 64 KB / 68 KB / 128 KB / 129 KB 时分别 4482 / 4517 / 4168 / 4484 / 4482 拍——相距 64 KB 整数倍（同 L2 set）并不特别慢。所以 store 每 beat 1.03–1.16 拍的波动不是 set 别名，而是与地址的关系不规则；结合 10.1 的 MSHR 占用数据，最可能的机制是 InclusiveCache 对 PutPartial 的读-改-写在 `BankedStore` 子 bank 上的冲突，而子 bank 由行所在的 **way** 参与决定，way 由替换历史决定、不由地址决定，模型无法（也不值得）复现。模型保留 `store_beat_cycles` 作为单位步长 store 的平均代价：RTL 配置取区间中值 1.10（此前 1.15 是按未对齐数据拟合的上限）。

### 10.9 4 lane RTL 与 L2 store 通路模型

新增 Chipyard 配置 `HwachaL4RocketConfig = WithNLanes(4) ++ HwachaRocketConfig`（增量构建 5 分钟）。4 lane 微基准（`rtl/results/micro-n4096-l4.log`）：计算类严格 4×（micro_alu 2076 vs 1 lane 8223，fdiv 3148 vs 12362）；load 与 1 lane 相同（2141）；但 store 变慢：micro_store 2461（1 lane 2117–2377、2 lane 2232），混合 load/store 4560–4630（1 lane 4180–4520），跨步 store 6086（1 lane 4207，每元素 1.49 拍）。

4 lane 的 TileLink 跟踪（`tlv-micro-n4096-l4.log`）给出机制：四条 lane 按 64 B 行交错，A 通道严格轮流（每个请求平均等 3.0 拍），load 仍是每拍 1 个；store 只有 0.86 beat/拍，L2 的 store 延迟 19.5 拍（1 lane 15.1）、MSHR 均值 8.3（1 lane 5.3）、A 通道被 L2 反压 15%（1 lane 5%）；跨步 store 0.69 beat/拍、延迟 40 拍。区别在于连续两个 store beat 是否落在同一行：1 lane 74% 是同一行（排进同一个 MSHR，便宜），4 lane 只有 2%（每个 beat 都要新分配 MSHR、做读-改-写）。

据此把 store 的附加代价从 lane 的 VMU 端口（`store_beat_cycles`，每 lane 一份，多 lane 时会被放大 lane 倍）移到 **L2 bank 的 store 通路**（`cc/src/mem.cc` `L2Bank`，所有 lane 共享）：每个 store beat 占 `l2_store_beat_cycles`（1.0）拍，与上一个 store beat 不在同一行时加 `l2_store_switch`（0.16），若还是部分写（小于 16 B 的跨步/索引 store）再加 `l2_partial_store_switch`（0.33），分数按累计信用折算成整拍。跨步/索引 store 的请求大小改为元素大小以便 L2 区分部分写。三个 RTL 配置的 `store_beat_cycles` 回到 1.0。

改动后的 C++ 模型误差（同一组参数）：

| | 1 lane | 2 lane | 4 lane |
|---|---|---|---|
| 完整基准 8 个内核，平均 / 最大 | 2.5% / 6.7%（sfilter） | 2.6% / 4.7%（dgemm） | 1.9% / 4.8%（dgemm） |
| 微基准，平均（不含 micro_empty） | 2.2% | 4.8% | 2.3% |
| micro_store / sstride | +2.3% / +4.2% | +8.0% / — | −2.0% / +0.9% |

4 lane 完整基准（`rtl/results/rtl-n4096-l4.log`）：

| kernel | 1 lane RTL | 2 lane RTL | 4 lane RTL 冷 | 4 lane RTL 稳态 | C++ | 误差 |
|---|---|---|---|---|---|---|
| vvadd | 6552 | 6260 | 7710 | 6655 | 6507 | −2.2% |
| saxpy | 3255 | 3335 | 4141 | 3298 | 3271 | −0.8% |
| daxpy | 6505 | 6645 | 7468 | 6637 | 6507 | −2.0% |
| csaxpy | 3859 | 3934 | 4705 | 3732 | 3784 | +1.4% |
| sfilter | 5264 | 5429 | 5947 | 5406 | 5351 | −1.0% |
| gather | 8543 | 8766 | 9946 | 8735 | 8548 | −2.1% |
| dgemm_opt | 8402 | 4776 | 5091 | 4544 | 4324 | −4.8% |
| fma_peak | 12309 | 6165 | 3095 | 3093 | 3128 | +1.1% |

访存内核 1→2→4 lane 完全不加速（vvadd 甚至从 6552 变 6655）；fma_peak 严格 4×；dgemm 4 lane 只有 1.85×（每次 vf 流入的 B 行被 4 条 lane 分摊后每 lane 的块太短，vf 开销占比上升，模型对此略偏乐观 −4.8%）。micro_empty 在 2/4 lane 下模型偏快 17%–26%（4 lane RTL 186 拍 vs 模型 137），是每次 vf 的固定开销在多 lane 时略高，对真实内核影响不到 1%。

### 10.10 8 lane 与 16 lane RTL：store 吞吐随并发行数下降

新增 `HwachaL8RocketConfig`、`HwachaL16RocketConfig`（增量构建 14 分钟 / 1 小时 40 分钟；16 lane 仿真器每周期约 5 倍慢于 1 lane）。微基准（稳态，`rtl/results/micro-n4096-l8.log`、`micro-n4096-l16.log`）：

| kernel | 1 lane | 2 lane | 4 lane | 8 lane | 16 lane | 说明 |
|---|---|---|---|---|---|---|
| micro_alu | 8223 | 4155 | 2076 | 1052 | 540 | 严格随 lane 数缩放 |
| micro_fdiv_d | 12362 | — | 3148 | 1610 | 842 | 同上 |
| micro_load | 2144 | 2121 | 2141 | 2141 | 2140 | 每拍 1 beat，与 lane 数无关 |
| micro_store | 2117 | 2232 | 2461 | 3579 | 4037 | 每 beat 1.03 → 1.09 → 1.20 → 1.75 → 1.97 拍 |
| micro_copy | 4474 | 4246 | 4633 | 5641 | 6105 | |
| micro_sstride（4 B 跨步 store） | 4207 | — | 6086 | 7227 | 8031 | 每元素 1.03 → 1.49 → 1.76 → 1.96 拍 |

8 lane 的 TileLink 跟踪（`tlv-micro-n4096-l8.log`）：load 仍是每拍 1 个（每个 lane 的请求平均等 7.0 拍，严格轮流）；store 0.59 beat/拍，L2 输入被反压的时间占 40%，MSHR 均值 8.3、峰值 10；store 延迟 24 拍（4 lane 19.5、1 lane 15）。机制是 InclusiveCache 对 PutPartial 的读-改-写：一条 lane 顺序写一行时四个 beat 连续到达、排进同一个 MSHR 后合并处理，几乎不多花时间；L 条 lane 交错时同时有 L 行各自在做读-改-写，数据阵列子 bank 冲突与 MSHR 占用随 L 增长，每 beat 代价从 1.05 拍升到接近 2 拍（读 + 写各一拍）后饱和。试过三种机械模型（命中 MSHR 池 + 行内串行服务、读-改-写合并窗口、按行内 beat 位置分子 bank 的冲突排队）都不能同时拟合 1–16 lane：模型里各 lane 的相位与 RTL 严格轮流的相位不同，冲突率对不上。最终采用**按并发行数查表**的经验模型（`cc/src/mem.cc` `L2Bank`，参数 `l2_store_conflict`）：每个 store beat 的附加代价 = 表(n) 按 log2(n) 插值，n = "当前行上一次出现以来经过的不同行数 + 1"（顺序流 n = 1，L 条 lane 交错访问不同行则 n = L；行首拍沿用上一拍的 n；窗口 `l2_store_window` = 32 个 beat）。RTL 配置的表为 `1:0.05, 2:0.09, 4:0.20, 8:0.75, 16:0.97`，部分写换行再加 `l2_partial_store_switch` 0.2。之前的换行代价（`l2_store_switch`）置 0。

同一组参数下 C++ 模型的误差：

| | 1 lane | 2 lane | 4 lane | 8 lane | 16 lane |
|---|---|---|---|---|---|
| micro_store | +3.3% | +1.6% | +1.3% | +1.0% | +0.4% |
| micro_copy / ldst | −5.1% / −2.3% | +1.6% / +3.0% | −2.0% / −0.5% | +0.3% / +0.4% | +0.1% / +0.2% |
| micro_sstride | +5.6% | — | −5.2% | +10.9% | +10.9% |
| micro_load | −2.2% | −1.5% | −2.4% | −2.4% | −2.3% |
| 完整基准平均 / 最大 | 2.5% / 6.9% | 2.4% / 4.7% | 1.8% / 4.8% | 2.5% / 4.8% | 1.3% / 4.2% |

8 lane 完整基准（`rtl/results/rtl-n4096-l8.log`）：

| kernel | 4 lane RTL | 8 lane RTL 冷 | 8 lane RTL 稳态 | C++ | 误差 |
|---|---|---|---|---|---|
| vvadd | 6655 | 8419 | 7717 | 7695 | −0.3% |
| saxpy | 3298 | 4148 | 3380 | 3229 | −4.5% |
| daxpy | 6637 | 8335 | 7736 | 7695 | −0.5% |
| csaxpy | 3732 | 4744 | 3914 | 3742 | −4.4% |
| sfilter | 5406 | 6001 | 5474 | 5313 | −2.9% |
| gather | 8735 | 10739 | 9794 | 9748 | −0.5% |
| dgemm_opt | 4544 | 4907 | 4438 | 4225 | −4.8% |
| fma_peak | 3093 | 1559 | 1557 | 1592 | +2.2% |

C++ 平均 |误差| 2.5%，最大 4.8%。8 lane 只有 fma_peak（1557，8×）与 dgemm（4438）继续加速，访存内核全部比 4 lane 慢（vvadd 6655 → 7717）。

16 lane 完整基准（`rtl/results/rtl-n4096-l16.log`）：

| kernel | 8 lane RTL | 16 lane RTL 冷 | 16 lane RTL 稳态 | C++ | 误差 |
|---|---|---|---|---|---|
| vvadd | 7717 | 9064 | 8182 | 8149 | −0.4% |
| saxpy | 3380 | 4223 | 3466 | 3450 | −0.5% |
| daxpy | 7736 | 8918 | 8179 | 8149 | −0.4% |
| csaxpy | 3914 | 4759 | 3919 | 3964 | +1.1% |
| sfilter | 5474 | 5990 | 5482 | 5458 | −0.4% |
| gather | 9794 | 11295 | 10233 | 10203 | −0.3% |
| dgemm_opt | 4438 | 4868 | 4376 | 4193 | −4.2% |
| fma_peak | 1557 | 791 | 789 | 814 | +3.2% |

C++ 平均 |误差| 1.3%，最大 4.2%。fma_peak 789 拍（16×），dgemm 4376（只比 8 lane 快 1.4%：每次 vf 的 B 行被 16 条 lane 分摊后每 lane 只剩几个元素，vf 开销与流水填充占主导），访存内核比 8 lane 再慢 1%–6%。

micro_empty 在 8/16 lane 下模型偏快 40%–59%（RTL 122 / 100 拍 vs 模型 73 / 41）：每次 vf 的固定开销随 lane 数略增，对真实内核影响不到 1%，未单独建模。

### 10.11 第三轮之后的误差汇总（C++ 模型）

- 完整基准（8 个内核，数组对齐）：1 lane 平均 2.5%（最大 6.9% sfilter）、2 lane 2.4%（最大 4.7%）、4 lane 1.8%（最大 4.8%）、8 lane 2.5%（最大 4.8%）、16 lane 1.3%（最大 4.2%）。
- 微基准（15 个）：1 lane 2.8%、4 lane 3.9%、8 lane 4.9%、16 lane 7.4%（最大值都是只有几十到几百拍的 micro_empty）；2 lane（10 个）4.7%。
- Rodinia 内核（5 个，踪迹驱动）：平均 4.2%，最大 9.2%（nn）。
- hwacha-cc bench 内核（5 个，踪迹驱动，8.1 节）：−3%～−7%，divloop −12%（未重跑）。

## 9. 下一步

1. store 通路按并发行数查表（10.10）是经验拟合：能同时覆盖 1–16 lane，但没有还原 InclusiveCache 读-改-写冲突的具体机制；1 lane 下 1.03–1.16 拍/beat 的布局波动（10.8）也无法复现。
2. nn −9%、pathfinder −6%：都是分支多、谓词化访存多的块，模型的一致性分支（每 strip 6 拍）与谓词归约可能仍偏乐观；divloop −12% 同类。
3. Python 模型从第三轮末起不再迭代（10.9 的 L2 store 通路、VSDQ 按数据量计数等只在 C++ 里），只作为 C++ 模型的交叉参照。
4. 冷启动效应（L1D 脏行/共享行探测，冷态比稳态慢 1%–40%）未建模：模型对应稳态。
5. VRU 与混合精度在开源 RTL 上仍没有可用配置，只能对论文数据做趋势校验；`25-design-space.md` 的多 lane/多 bank 结论是模型外推。
