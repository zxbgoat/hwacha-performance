# 09 主序列器与 lane 序列器

源码：`sequencer-master.scala`（`MasterSequencer`、`SeqParameters`）、`sequencer-lane.scala`（`LaneSequencer`）、`types-vxu.scala`（`SeqEntry`、`MasterSeqEntry`、`IssueOp`）

## 1. 序列器在做什么

序列器是 Hwacha 的**发射窗口**。每条向量指令在发射时占据一个（或多个）序列器槽位，直到所有 lane 把全部元素执行完才释放。序列器负责：

- 记录每个在途向量操作的类型、寄存器、标量操作数；
- 相对于更老的在途操作动态计算 RAW/WAR/WAW 冒险；
- 每周期在每个 lane 里挑出一个无冒险、资源可用的操作发给展开器，粒度是一个 strip；
- 跟踪每个操作在每个 lane 中的进度（剩余 vlen、当前元素索引）；
- 处理与变延迟单元（除法、访存）之间的簿记操作。

它像一个乱序发射窗，但**从不投机**：只有冒险完全清除才发射，也没有回滚。

## 2. 主序列器与 lane 序列器的分工

```
                 主序列器（全局唯一，nSeq = 8 槽）
   valid | operation | base-p/rs1/rs2/rs3/rd | scalar-rs1/rs2/rs3 | func | misc | hazard-raw/war/waw
                          │ 同步发射到所有 lane
        ┌─────────────────┼───────────────────┐
        ▼                 ▼                   ▼
  Lane 0 序列器     Lane 1 序列器      …  Lane N 序列器   （各 nSeq 槽，与主序列器槽一一对应）
   phys-p/rs1/rs2/rs3 | vlen | eidx | age  → 调度器 → 展开器
```

**主序列器**保存所有 lane 共用的静态信息：

| 字段 | 用途 |
|---|---|
| operation（`active` 位图） | 需要哪些功能单元/簇：viu、vimu、vipu、vfmu0/1、vfcu、vfvu、vpu、vsu、vgu、vqu、vidu、vfdu、vcu、vlu |
| base-p、base-rs1/rs2/rs3、base-rd | 架构寄存器号（含类型：向量/共享/谓词），用于对**尚未发射**的后续指令做冒险检测 |
| scalar-rs1/rs2/rs3 | 标量操作数的值（来自 vs） |
| func、misc | 功能码、精度、舍入模式、访存类型等 |
| hazard-raw/war/waw | 相对**已发射**更老操作的依赖位图 |

主序列器还维护 head 指针和 `vf.last`（当前块最后一条指令退休），后者通知 VRU。它输出 `pending.mem`/`pending.all` 供 fence 使用，`master.state` 供 MOU 使用。

**lane 序列器**保存本 lane 的动态信息：

| 字段 | 用途 |
|---|---|
| phys-p、phys-rs1/rs2/rs3、phys-rd | 当前 strip 对应的物理 SRAM 行号（架构号 × vstride + base + strip 偏移） |
| vlen | 本 lane 剩余元素数 |
| eidx | 本 lane 当前元素索引 |
| age | 相对 head 的年龄 |

发射时 valid 置位，vlen 载入该 lane 分到的长度，`hazard-*` 由主序列器比较新指令的寄存器与已发射操作的 base 寄存器得到。槽位不足时标量单元停顿。所有 lane 都完成（`mseq.clear`）后主序列器释放槽位。

## 3. 序列器操作类型

一条工作线程指令可能对应多个序列器操作（各占一槽，须按固定顺序分配）：

| 序列器操作 | 动作 |
|---|---|
| VIU | 整数 ALU（bank 本地） |
| VIMU | 整数乘 |
| VIPU | 谓词逻辑（bank 本地 PLU） |
| VFMU | 浮点 FMA（VFU0 的 FMA0 或 VFU1 的 FMA1） |
| VFCU | 浮点比较 |
| VFVU | 浮点转换 |
| VPU | 谓词读出到 BPQ（供 VMU 生成掩码） |
| VSU | 寄存器读出到 BRQ（store 数据 / AMO 数据） |
| VGU | 谓词/寄存器读出到 VGU 的 LPQ/LRQ（gather/scatter/AMO 地址） |
| VQU | 谓词/寄存器读出到 VFU2 的 LPQ/LRQ（除法、开方、归约） |
| VIDU | 整数除法簿记 |
| VFDU | 浮点除法/开方簿记 |
| VCU | VMU 地址翻译簿记：翻译成功后才放行后续 VSU/VLU |
| VLU | 向量 load 写回簿记：跟踪从 BWQ 写回 VRF 的元素数 |

指令到序列器操作的映射：

| 指令类别 | 序列器操作 |
|---|---|
| 向量整数计算 | VIU |
| 向量整数乘 | VIMU |
| 向量整数除 | VQU + VIDU |
| 向量整数归约（vfirst） | VQU |
| 向量谓词计算 | VIPU |
| 向量谓词归约（一致性分支） | VQU |
| 浮点 FMA | VFMU |
| 浮点除/开方 | VQU + VFDU |
| 浮点比较 | VFCU |
| 浮点转换 | VFVU |
| 向量 AMO | VGU + VCU + VSU + VLU |
| 索引 load | VGU + VCU + VLU |
| 索引 store | VGU + VCU + VSU |
| 常量/单位步长 load | VPU + VCU + VLU |
| 常量/单位步长 store | VPU + VCU + VSU |

簿记类操作（VIDU、VFDU、VCU、VLU）不产生 bank µop，不经过展开器。

## 4. lane 调度器的冒险检查

源码把冒险分为三类，每类对每个槽位输出一个位：

**数据冒险 dhazard**
- `dhazard_raw_vlen`：与更老操作的进度比较（更老操作的剩余 vlen 必须已推进到本操作要读的 strip 之前）。
- `dhazard_raw_{pred_}vs1/vs2/vs3/vp`：对每个源操作数检查展开器 ticker（移位寄存器）中是否有尚未落地的写。
- `dhazard_war`、`dhazard_waw`：目的寄存器与更老操作的读/写冲突。`nRPorts <= 3` 的约束（源码 `require`）让 WAR 检查不必对照 SRAM 读 ticker。

**bank 冒险 bhazard**：本操作要用的读/写端口是否与展开器中已排好的 µop 在同一时刻撞上同一 bank。检查时向右移一位，因为看的是下一周期。

**结构冒险 shazard**：功能单元、交叉开关、操作数锁存器、谓词交叉开关的占用掩码（`use_mask_*`）与本操作的请求掩码是否重叠。

三者都清除，且对应的下游队列有空间（DCC 的 look-ahead 计数器允许）才 `consider`。调度器从 head 开始 `find_first` 挑最老的可发射操作（age 仲裁），有两个调度端口（`first_sched`、`second_sched`）以及若干独立端口（VDU/VGU/VSU 的 look-ahead 预约）。

## 5. 发射一个 strip 之后

- `update_eidx`：eidx += strip 元素数，vlen −= strip 元素数。
- `update_vs/vd/vp`：物理寄存器号按 `vstride`/`pstride` 步进到下一 strip 所在的行。
- 混合精度下还要 `update_pack`（子字索引）与 `step_pstride`。
- vlen 归零后槽位在本 lane 标记完成；所有 lane 完成后主序列器清除。

## 6. 参数

| 参数 | 值 | 说明 |
|---|---|---|
| nSeq | 8 | 槽位数 |
| nRPorts | 3 | 一个操作最多 3 个向量源操作数 |
| expLatency | 1 | 展开器延迟 |
| maxWPortLatency | 3 + 1 + 1 + max(各功能单元级数) | 写口 ticker 深度 |
| maxPredWPortLatency | 1 + max(PLU, 2+1+ALU, 2+1+FCmp) | 谓词写口 ticker 深度 |
| maxLookAhead | max(TileLink 位宽/8, nStrip) | DCC 计数器预约上限 |
