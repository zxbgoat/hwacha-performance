# 12 向量功能单元

源码：`vfu-alu.scala`、`vfu-plu.scala`、`vfu-imul.scala`、`vfu-fma.scala`、`vfu-fconv.scala`、`vfu-fcmp.scala`、`vfu-fdiv.scala`、`vfu-idiv.scala`、`vfu-rfirst.scala`、`vfu-rpred.scala`

## 1. 三类功能单元

| 类别 | 单元 | 位置 | 延迟 | 结果去向 |
|---|---|---|---|---|
| bank 本地 | ALU、PLU | 每 bank 每 slice | 固定（ALU 默认 1 级，PLU 0 级） | 直接写本 bank |
| 共享定延迟 | FMA0、IMul、FConv（VFU0）；FMA1、FCmp（VFU1） | 每 lane，按 slice 例化 | 固定流水 | 结果扇出到所有 bank，由展开器预排的 sram-write / pred-write 写回 |
| 共享变延迟 | FDiv/FSqrt、IDiv、RPred、RFirst（VFU2，在 DCC 的 VDU 内） | 每 lane | 可变 | 经 BWQ.fu 解耦写回，或送回标量单元 |

## 2. 定延迟单元

**ALUSlice**：64/32 位整数加减、逻辑、移位、slt/sltu、veidx（元素索引左移）。也产生比较结果给 PRF（`vcmpeq/lt/ltu`）。每 slice 一个，编号 `bid*nSlices + i`，用于 veidx 计算元素索引。

**PLUSlice**：三输入谓词逻辑，输入三个谓词与 8 位真值表，输出一个谓词。VIPU 是唯一写 PRF 的序列器操作。

**IMulSlice**：64 位整数乘（mul/mulh/mulhu/mulhsu/mulw），默认 3 级流水。

**FMASlice**：基于 hardfloat 的融合乘加，支持 d/s/h 三种精度以及加宽形式。流水级数分别为 `HwachaStagesDFMA = 4`、`SFMA = 3`、`HFMA = 3`。每 lane 两个 FMA 簇（`nVFMU = 2`），每簇按 slice 例化，因此单 lane 每周期 4 个双精度 FMA。启用混合精度后每簇额外例化 2 个单精度和 6 个半精度 FMA 以维持满吞吐（见 `16-mixed-precision.md`）。

**FConvSlice**：浮点精度互转、浮点与整数互转，默认 2 级。

**FCmpSlice**：浮点比较（feq/flt/fle）、fmin/fmax、fsgnj*、fclass，默认 1 级。比较结果写入 PRF（`wpred`），其余写 VRF。

所有定延迟单元的写回时刻由展开器精确排定，序列器只需在 ticker 中检查冲突。

## 3. 变延迟单元

变延迟单元不能预排写回，因此走**解耦路径**：序列器用 VQU 操作把操作数/谓词读到 LRQ/LPQ，VDU 从队列取数送入单元，结果进入输出队列，再通过 BWQ.fu 写回 VRF；序列器的 VIDU/VFDU 簿记操作跟踪写回进度。

**FDivSlice**：hardfloat `DivSqrtRecF64`。所有精度先转换为 recoded double（`RecFNToRecFN`）计算，再转回目标精度。输入两级队列（in0q、in1q）加 tag 队列，输出 `rq` 队列，`QCounter` 控制在途数不超过 `nDecoupledUnitWBQueue`。

**IDivSlice**：复用 Rocket 的 `MulDiv`（div/divu/rem/remu 及 32 位变体），同样带结果队列与计数器。

## 4. 归约单元

Hwacha 的归约只有两种，都要跨 lane 汇总，因此分为 lane 级与 master 级：

**RPredLane / RPredMaster**：谓词归约，用于一致性分支 `vcjal.{all,any}`。每个 lane 对本地谓词做 all/any，`RPredMaster`（顶层例化，opq 深度 2）汇总所有 lane 的结果后送回标量单元的执行级解析分支。

**RFirstLane / RFirstMaster**：`vfirst`。每个 lane 找出本地第一个活跃元素的值与索引，`RFirstMaster` 按元素索引挑出全局第一个，结果通过标量单元的长延迟写回仲裁器写入 vs，scoreboard 在结果回来前互锁。

归约操作会让主序列器把所有 lane 同步起来，是 lane 之间唯一的同步点。

## 5. 流水级数参数

| 参数 | 默认 |
|---|---|
| HwachaStagesALU | 1 |
| HwachaStagesPLU | 0 |
| HwachaStagesIMul | 3 |
| HwachaStagesDFMA | 4 |
| HwachaStagesSFMA | 3 |
| HwachaStagesHFMA | 3 |
| HwachaStagesFConv | 2 |
| HwachaStagesFCmp | 1 |

`SeqParameters` 要求所有定延迟单元至少 1 级。改变级数会自动改变展开器 ticker 深度与序列器冒险窗口。

## 6. 45nm 芯片的差异

EOS 芯片上浮点单元与 Rocket 共享以节省面积；第四代每个 lane 拥有私有的浮点单元，只有标量单元的浮点操作仍走 Rocket FPU。
