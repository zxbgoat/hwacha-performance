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

## 8. 下一步

1. 混合访存的 store 代价：用 `+verbose` 的 TileLink 通道跟踪确认 InclusiveCache 对 Put 的处理周期，把它作为 L2 的写占用参数而不是端口占用。
2. 未对齐单位步长访存的 beat 合并：按 RTL 的 VMU 边界处理重写 `beatsFor`。
3. 冷启动效应：为 `warm_l2` 增加"L1D 中共享/脏行"的可选描述，复现 `_warm`/冷两列。
4. 多 lane、VRU、混合精度在开源 RTL 上都没有可用配置（VRU 未接入，多 lane 配置未验证），这部分仍只能对论文数据做趋势校验。
