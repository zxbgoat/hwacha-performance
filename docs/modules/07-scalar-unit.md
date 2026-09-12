# 07 标量单元、SMU 与共享 FPU 接口

源码：`scalar-unit.scala`、`scalar-decode.scala`、`smu.scala`、`scalar-fpu-interface.scala`、`scalar-fpu.scala`、`frontend.scala`

## 1. 标量单元的位置

标量单元（论文中称 SXU）是 Hwacha 的**向量前端**：它从 VCMDQ 取控制线程命令，对 `vf` 启动从 L1 VI$ 的取指，逐条译码工作线程指令，然后：

- 标量整数算术：本地 4 级流水执行；
- 标量整数乘除：解耦的 MulDiv 单元（复用 Rocket 的 `MulDiv`，mulUnroll = 8，early-out）；
- 标量访存：交给 SMU；
- 标量浮点：通过 FPREQQ/FPRESPQ 使用 Rocket 的 FPU（或可选的本地 FPU）；
- 向量指令：送主序列器和所有 lane；
- 向量归约/一致性分支：送 RPredMaster/RFirstMaster，并等待结果。

## 2. 状态

| 状态 | 说明 |
|---|---|
| `SRegFile` | 共享寄存器堆 vs1–vs63（vs0 不存，读出恒 0） |
| 地址寄存器堆 va0–va31 | 只由 `vmca` 写，块内只读，因此译码时无需检查 WAR |
| `Scoreboard(nSRegs)` | 每个 vs 一位，乘除/访存/FPU/归约等长延迟操作写目的寄存器前置位，结果回来时清除 |
| `vf_active` | 当前正在执行向量块；`vstop` 清除 |
| `vl` | 每 lane 的向量长度条目（多 lane 时各 lane 分到的元素数） |
| `MemTracker(4,4)` | 跟踪标量单元自身未完成的 load/store，供 MOU 与 fence 使用 |
| `pending_smu` / `pending_cbranch` | 一次只允许一个未完成的标量访存；一致性分支未解析前停顿 |
| `id_status` | 发出 vf 时的 mstatus，用于 TLB 特权级 |

## 3. 流水线

```
 PC 生成 ──▶ VI$ 访问 (ITLB) ──▶ 译码 (读 vs/va，冒险检查，分发) ──▶ 执行 (ALU / 分支解析) ──▶ 写回
```

**取指**：`io.imem.active := vf_active`。`vf` 命令携带目标 PC，前端从 4 KB 两路组相联（Chipyard 默认配置为 64 组 × 1 路 × 8 字节行，即每行一条指令）VI$ 取 64 位指令。VI$ 与 VRU 共享（`HwachaFrontend` 有 vxu 与 vru 两个请求端口），这也让 VRU 顺便为标量单元预热指令缓存。

**译码**：用 `scalar-decode.scala` 的大译码表得到 `IntCtrlSigs`。停顿条件（`ctrl_stalld_common`）包括：

- `!vf_active`；
- 与执行级的 RAW（`id_ex_hazard`）；
- scoreboard 命中（源或目的 vs 有未返回的长延迟结果）；
- 有未完成的标量访存（`stall_smu`）；
- `vfence` 且主序列器仍有访存操作未退休、或任一 MRT 仍有在途访存（`stall_pending_fence`）；
- 下游 `stallx`/`stallw`。

还有针对目标单元的 ready 条件：主序列器有空槽、DCC 能接收、VMU 命令队列能接收、FPU/SMU/MulDiv 请求通道 ready。

**向 VMU 发命令**：译码级直接为向量访存构造 VMU 操作：基址取自 va（单位/常量步长）、0（AMO，地址来自向量寄存器）或 vs（索引访存）；步长取自 va（常量步长）或元素宽度（单位步长）。

**执行**：本地 ALU；解析一致性分支（`vcjal`/`vcjalr` 的 all/any 判定来自 RPredMaster 汇总的谓词归约结果），分支未解析前译码停顿，不做投机。四种长延迟结果（FPU、SMU load、MulDiv、vfirst）在执行级若同时返回会引起停顿。

**写回**：一个 4 路仲裁器 `ll_warb` 合并 FPU、SMU、MulDiv、RFirst 四种长延迟写回，长延迟写口优先于流水线写口；写回时清除 scoreboard。断言保证不会在 scoreboard 置位时写同一 vs。

## 4. 向量指令的发射

译码级把向量指令打包成 `IssueOp`（含操作类型、base 寄存器号、各 lane 的 vlen、标量操作数值、功能码），一次性发给：

- 主序列器（`mseq.io.op`）；
- 每个 lane 的 VXU（`vus(i).io.issue.vxu`）；
- 若是访存指令，还发给每个 lane 的 VMU（`issue.vmu`）；
- 若是谓词归约或 vfirst，发给 RPredMaster / RFirstMaster。

所有目标必须同时 ready 才 fire（`fire_vxu`/`fire_vmu` 用排除式 ready 组合）。

## 5. 标量访存单元 SMU

- 独立的 `LazyModule`，自带 TileLink master 节点（16 字节宽度适配）和私有 TLB（`nptlb` 组 × 1 路，源码复用参数名）。
- 内部 `Table(nSMU = 16)` 记录在途请求的 tag 与元数据，`TBox(1)` 负责 TLB 请求/应答。
- 支持 b/h/w/d 四种宽度和符号/零扩展 load。
- 标量单元一次只发一个 SMU 请求（`pending_smu`），响应经 `ll_warb` 写回 vs。
- store 完成通过 `confirm` 信号通知。

## 6. 共享 FPU 接口

`HwachaLocalScalarFPU = false`（且源码中 `local_sfpu` 被硬编码为 false）时，`ScalarFPUInterface` 把标量单元的 FPU 请求打包成 RoCC 的 `fpu_req`，送到 Rocket 的 FPU，结果由 `fpu_resp` 回来。舍入模式：指令静态指定则用指令的，动态则用 Rocket 的 frm（源码 TODO 注释：需要把 Rocket 的 rm 管过来）。浮点异常标志累积到 Rocket 的 fflags。

`ScalarFPU` 是备用的本地 FPU 实现，当前未接入。

## 7. 与 VRU、MOU 的交互

- `vf_active` 输出给 RoCC 单元（busy 判断）。
- `pending.mrt.su` 输出给 MOU，MOU 返回 `mocheck.su.{load,store}` 决定标量访存是否可以发出（默认严格排序：标量 load 需无在途向量 store，标量 store 需无任何在途向量访存）。
- `vf_stop` 通知主序列器当前块结束，主序列器最后一条指令退休时向 VRU 发块完成应答。
