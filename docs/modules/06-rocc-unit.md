# 06 RoCC 单元与命令队列

源码：`rocc-unit.scala`（`RoCCUnit`、`CMDQ`、`HwachaDecodeTable`）、`hwacha.scala`（`Hwacha`、`HwachaImp`）

## 1. 职责

RoCC 单元是 Hwacha 与 Rocket 控制核之间的唯一控制接口。它：

1. 接收 Rocket 在 commit 阶段发来的控制线程指令（RoCC `cmd` 通道，含指令字、rs1、rs2、status）。
2. 译码并把指令分发到两条命令队列：VCMDQ（给标量单元）和 VRCMDQ（给 VRU）。
3. 维护配置状态：vcfg、最大 HVL、当前 vlen、lane stride、各精度寄存器的物理基址与步长。
4. 对需要返回值的指令（`vsetvl`、`vgetcfg`、`vgetvl`、异常查询）通过 RoCC `resp` 通道回写 rd。
5. 输出 `busy` 供 Rocket 的 fence 使用，输出 `interrupt`。

## 2. 指令分发表

| 控制线程指令 | 入 VCMDQ | 入 VRCMDQ | RoCC 应答 | 说明 |
|---|---|---|---|---|
| vsetcfg | Y | Y | N | 重算 maxvl，vlen 清零 |
| vsetvl | Y | Y | Y（新 vl） | vl = min(rs1, maxvl) |
| vgetcfg | N | N | Y | 直接从配置寄存器应答 |
| vgetvl | N | N | Y | |
| vuncfg | N | N | N | |
| vmcs | Y | N | N | VRU 不关心共享寄存器 |
| vmca | Y | Y | N | VRU 需要地址寄存器副本 |
| vf / vft | Y | Y | N | 携带目标 PC 与 status |

源码译码表中还有一组特权级的异常/保存恢复指令：`vxcptcause`、`vxcptaux`（应答异常原因与辅助信息）、`vxcptsave`、`vxcptrestore`、`vxcptkill`。它们是 OS 处理可重启异常与上下文切换的接口（见 `15-memory-ordering-exceptions.md`），译码表里 save/restore/kill 的控制位在当前代码中标注为 unused。

## 3. 命令队列结构

`CMDQ` 不是单一队列，而是五条并行子队列，深度都由 `HwachaCMDQLen`（默认 32）决定，并要求不小于 bank 数：

| 子队列 | 内容 | 谁入队 |
|---|---|---|
| cmd | 译码后的命令类型 | 所有入队指令 |
| imm | 立即数/数据：vsetvl 的 vlen、vmcs/vmca 的 rs1、vf 的目标地址 | 按 `sel_imm` 选择 |
| rd | 目的寄存器号（vs/va 编号） | vmcs、vmca |
| cnt | 计数 | 预留 |
| status | 发出 vf 时控制核的 mstatus | vf |

标量单元和 VRU 各自从自己的 CMDQ 出队。VRU 未启用时（当前开源代码 `confvru = false`），VRCMDQ 的 ready 恒为真，相当于直接丢弃。

## 4. HVL 的计算

`vsetcfg` 到达时：

1. 从 vcfg 取出 nvvd、nvvw、nvvh（64/32/16 位向量寄存器数）和 nvp（谓词寄存器数）。
2. 计算每个 bank 能容纳的元素数 epb：未启用混合精度时 `nvv = nvvd + nvvw + nvvh`，用查找表得到 `256 / nvv` 类的值；启用混合精度时按 `4·nvvd + 2·nvvw + nvvh` 加权（一个 128 位 SRAM 行放 2 个 d、4 个 w 或 8 个 h）。谓词寄存器堆也有对应查找表，二者取最小。
3. `maxvl = epb << (log2 nLanes + log2 nBanks + log2 nSlices)`，也就是 epb × 8 × nLanes。
4. 同时记录各精度区域在 SRAM 中的物理基址（`cfg.base`）、步长（`cfg.vstride.{d,w,h}` = 该精度寄存器数）、谓词步长（`pstride = nvp`），供序列器把架构寄存器号映射到物理行。
5. `unpred := (nvp == 0)`：内核不用谓词时硬件可跳过谓词读取。

复位值 maxvl = 8，与 ISA 保证的最小 HVL 一致。

`HwachaVSETVLCompress`（默认 true）：连续两条参数相同的 `vsetvl` 只处理一次，减少命令队列占用。

## 5. 配置总线 HwachaConfigIO

RoCC 单元把配置广播给标量单元、主序列器、各 lane 的序列器/展开器/lane/DCC/VMU 与 MOU：

| 字段 | 含义 |
|---|---|
| valid | 配置有效（混合精度模式下 vsetcfg 需要多周期，此时为 false） |
| morelax | 内存排序放松开关（当前恒 false） |
| unpred | 内核未使用谓词 |
| lstrip / lstride | 多 lane 元素交错粒度（strip 的 2^lstride 倍） |
| pstride、vstride.{d,w,h} | 谓词/各精度向量寄存器的物理步长 |
| base | 各精度区域物理基址 |
| id.{vp,vd,vw,vh} | 各类寄存器号上界，用于非法指令检查 |

## 6. 顶层 Hwacha 的连接

`HwachaImp` 例化：RoCCUnit、ScalarUnit、MasterSequencer、RPredMaster、RFirstMaster、MemOrderingUnit，以及 LazyModule 形式的 HwachaFrontend（VI$）、SMU、nLanes 个 VectorUnit 和可选的 VRU。TileLink 侧：VI$、SMU、VRU 挂在一个内部 `atlBus` 交叉开关上，经 16 字节宽度适配器接到 RoCC 的 `atlNode`；每个 VectorUnit 的 master 节点各自经宽度适配器接到 `tlNode`。RoCC 的 `io.mem`（L1D 端口）被置为无效，Hwacha 完全不走 L1D。

PTW 端口分配：`ptw(0)` 给 VI$ 的 ITLB，`ptw(1)` 给 SMU 的 TLB，其余 nLanes 个给各 lane 的 DTLB。
