# 10 展开器与 bank 微操作

源码：`expander.scala`（`Expander`、`TickerIO`、`ExpParameters`）、`types-vxu.scala`（各类 `*LaneOp`、`*ExpEntry`）

## 1. 职责

展开器把 lane 序列器发来的一个**序列器操作 + 一个 strip** 翻译成一组**bank 微操作（µop）**，并按功能单元的延迟把它们放进各自的移位寄存器（源码叫 ticker）。移位寄存器每周期下移一格，末端的 µop 送给 bank 0，然后 bank 0 → bank 1 → bank 2 → bank 3 逐拍传递。

因为读 µop 和写 µop 在移位寄存器中的位移差恰好等于功能单元延迟，整个 systolic 数据通路不需要任何动态互锁。

## 2. bank µop 列表

| µop | 动作 |
|---|---|
| sram-read | 读 VRF SRAM 一行 |
| sram-write | 选择写回 mux 并写 VRF |
| pred-read | 读 PRF |
| pred-write | 写 PRF（只有 VIPU 用） |
| opl | 把 SRAM 读出结果写入指定操作数锁存器 |
| pdl | 把 PRF 读出结果写入谓词锁存器 |
| sreg | 用给定的标量操作数代替寄存器读（来自 vs 的操作数广播到所有 slice） |
| xbar | 把操作数锁存器驱动到操作数交叉开关 |
| fop-alu | 用 bank 本地 ALU |
| fop-plu | 用 bank 本地 PLU |
| fop-brq | 把操作数写入本 bank 的 BRQ |
| fop-bpq | 把谓词写入本 bank 的 BPQ |
| fop-vfu0-fma0 / imul / fconv | 触发 VFU0 上的对应单元 |
| fop-vfu1-fma1 / fcmp | 触发 VFU1 上的对应单元 |
| fop-vfu2 | 写入 VFU2 的 LPQ/LRQ |
| fop-vgu | 写入 VGU 的 LPQ/LRQ |

## 3. 各序列器操作使用的 µop

| 序列器操作 | 读操作数数 | µop 组合 |
|---|---|---|
| VIU | 2 | sram-read、pred-read、opl、pdl、sreg、fop-alu、sram-write |
| VIMU | 2 | 同上 + xbar、fop-vfu0-imul |
| VIPU | – | pred-read、pdl、fop-plu、pred-write |
| VFMU | 3（乘为 2） | sram-read、pred-read、opl、pdl、sreg、xbar、fop-vfu0-fma0 或 fop-vfu1-fma1、sram-write |
| VFCU | 2 | … xbar、fop-vfu1-fcmp（结果写 PRF） |
| VFVU | 1 | … xbar、fop-vfu0-fconv、sram-write |
| VPU | – | pred-read、fop-bpq |
| VSU | 1 | sram-read、pred-read、opl、pdl、sreg、fop-brq |
| VGU | 1 | … xbar、fop-vgu |
| VQU | 2 | … xbar、fop-vfu2 |

规则：
- 只用 bank 本地单元的操作（VIU、VIPU、VPU、VSU）不用 xbar。
- 只碰谓词寄存器堆的操作（VIPU、VPU）不用 sreg。
- 若某个源操作数来自 vs，则该位置放 sreg 而不是 sram-read。
- VFMU 根据哪个 FMA 空闲选择 fma0 或 fma1。
- 变延迟操作（VQU 系列）不排 sram-write，结果由 VDU/VLU 经 BWQ 写回。

## 4. 例子：VFMU（三操作数 FMA）

序列器发来物理行 13、14、16 上的 FMA：

```
tick   0     1     2     3       4          …      3+L
      read13 read14 read16 xbar   fop-fma0          write(rd)
             opl0  opl1   opl2
```

三条 sram-read 相隔一拍进入 bank 0；每条读出的数据在下一拍由 opl 写入操作数锁存器 0/1/2；xbar 与 fop-fma0 同拍，把三个锁存器驱动到交叉开关并触发 FMA；sram-write 放在 fop 之后第 L 拍（L = FMA 流水级数，双精度默认 4）。同一组 µop 随后依次进入 bank 1、2、3，各 bank 处理自己的 2 个 slice。

## 5. ticker 深度

| ticker | 深度 |
|---|---|
| sram.read | nRPorts = 3 |
| sram.write | maxWPortLatency |
| pred.read / gread | 1 / 3 |
| pred.write | maxPredWPortLatency |
| sreg.global / local | nRPorts+2 / rpVIU+2 |
| xbar / pxbar | nRPorts+2 |
| viu、vimu、vfmu(×2)、vfcu、vfvu、vgu、vsu、vqu | 各自读操作数数 + 2 |
| vipu、vpu | 2 |

`rp*` 常量给出每类操作的向量读操作数上限：VIU 2、VIMU 2、VFMU 3、VFCU 2、VFVU 1、VGU 1、VSU 1、VQU 2。

## 6. 操作数锁存器分配

lane 有 6 个全局操作数锁存器（nGOPL）和 4 个全局谓词锁存器（nGPDL），固定分配给各共享功能单元（`lane.scala` 注释）：

| 单元 | 谓词锁存器 | 操作数锁存器 |
|---|---|---|
| VIMU | 0 | 0, 1 |
| VFMU0 | 0 | 0, 1, 2 |
| VFVU | 1 | 2 |
| VFMU1 | 2 | 3, 4, 5 |
| VQU | 2 | 3, 4 |
| VFCU | 2 | 3, 4 |
| VGU | 3 | 5 |

这一分配决定了哪些操作可以同时在飞：例如 FMA0 与 FMA1 可以并行，但 FMA1 与 FCmp 共用锁存器 3、4，不能同拍触发。序列器的结构冒险检查（`use_mask_*`）就是基于这张表。

## 7. 序列器与展开器的反馈

序列器不仅向展开器发操作，还**读取展开器的 ticker**：数据冒险检查要看 sram.write ticker 中是否有对本操作源寄存器的未完成写，bank 冒险检查要看下一拍各 bank 端口的占用。这就是论文说的"通过窥视展开器来清除冒险"，也是 chaining 得以自然产生的机制：相关操作的 µop 只要在 ticker 中不冲突就可以交错。
