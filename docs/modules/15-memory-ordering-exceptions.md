# 15 内存排序、fence、虚拟内存与可重启异常

源码：`mou.scala`（`MemOrderingUnit`）、`xcpt.scala`（`XCPT`）、`irq.scala`、`mrt.scala`、`rocc-unit.scala`（vxcpt* 译码）

## 1. 需要解决的问题

Hwacha 的控制线程与工作线程共享同一个虚拟地址空间，而向量访存与标量访存走不同路径（L1D 与 L2 直连），且向量单元内部多条访存可以乱序在飞。因此需要：

1. 在控制线程与工作线程之间、以及工作线程内部定义可靠的内存排序语义；
2. 处理向量访存的缺页与 TLB 缺失，让 OS 能像处理标量缺页一样处理它们；
3. 允许 OS 在向量单元忙时暂停、保存、恢复它，实现多道程序调度。

## 2. 内存排序单元（MOU）

MOU 在顶层例化，输入主序列器状态与所有 MemTracker 的 pending 信息，输出每个访存源是否允许发出的 `MOCheck{load, store}`。当前 `morelax` 恒为 false，即默认**严格排序**：

**标量访存（SMU）**
- 标量 load 可发出 ⇔ 没有在途的向量 store。
- 标量 store 可发出 ⇔ 没有任何在途的向量访存。

**向量访存（每 lane 每序列器槽）**
- 向量 load 可发出 ⇔ 没有在途标量 store，且（它是"第一个"向量访存操作，或其他 lane 没有在途向量 store）。
- 向量 store 可发出 ⇔ 没有在途标量访存，且（它是"第一个"，或其他 lane 没有任何在途向量访存）。

"第一个"的定义：从主序列器 head 起第一个带 VCU 的操作，且 head 与它之间没有正在发 acquire 的操作（`pending_addr`，考虑了槽位环绕）。这保证了同一 lane 内按程序序，不同 lane 之间在有 store 时串行化，同时允许纯 load 流水并发。

## 3. fence

- **控制线程**：RISC-V `fence` 会等待 RoCC `busy`；Hwacha 的 busy 由 RoCC 单元根据主序列器 pending 与所有 MRT pending 计算，因此标量核的 fence 会等到向量单元所有访存完成。
- **工作线程 `vfence`**：标量单元译码到 `vfence` 时停顿，直到主序列器没有未退休的访存操作且所有 MRT（标量单元自己的和各 lane 的）都没有在途访存。编码中含 pred/succ 字段但当前实现是全排序。
- 源码 TODO 注释：`vfence` 目前不刷新 VI$/TLB。

## 4. 虚拟内存

- 所有向量地址都是虚地址。每个 lane 的 VMU 有自己的 TLB（8 路），SMU 和 VI$ 也各有 TLB，都通过 RoCC 的 PTW 端口连到 Rocket 的页表遍历器。
- 发出 `vf` 时控制核的 mstatus 随命令进入 status 队列，作为向量访存的特权级/ASID 上下文。
- 单位步长访存按页粒度翻译，一次翻译覆盖整页内的元素。
- 序列器的 VCU 簿记操作保证：只有一个 strip 的所有地址都翻译成功，对应的 store 数据读出（VSU）或 load 写回（VLU）才会放行。这样任何翻译失败发生时，失败元素之后的操作还没有产生副作用。

## 5. 可重启异常

设计思路（来自 Vaidyanathan 等人的 OS-friendly accelerator 框架，与 DEC Vector VAX 类似）：不做精确异常，也不做寄存器重命名，而是允许多条向量指令部分完成，给 OS 一个**不透明的微架构状态保存/恢复**机制。

RoCC 单元译码表中的特权级指令：

| 指令 | 作用 |
|---|---|
| vxcptcause | 读异常原因 |
| vxcptaux | 读辅助信息（如出错地址） |
| vxcptsave | 把向量单元状态撤出到内存（evac） |
| vxcptrestore | 从内存恢复 |
| vxcptkill | 丢弃当前向量单元状态 |

`XCPT` 模块的状态机：

```
NORMAL ──exception──▶ XCPT_DRAIN（停顿顶层/发射/序列器/TLB，等展开器清空、MRT 无在途）
                          │
                          ▼
                     XCPT_FLUSH（flush top/vxu/vru/vmu；若 kill 则同时 flush_kill/flush_aiw）
                          │
                          ▼
                 XCPT_EVAC / XCPT_DRAIN_EVAC（若请求 evac，启动状态撤出到给定地址）
                          │
                          ▼
                       NORMAL
NORMAL ──hold──▶ HOLD（停顿发射与 TLB，直到 rocc.s 清除）
```

流程：向量访存发生缺页 → 向量单元向 Rocket 报 interrupt/exception → OS 通过 vxcptcause/vxcptaux 得到原因和地址 → 处理缺页（或决定切换进程时 vxcptsave）→ 恢复后重新执行。由于地址翻译在 VCU 处统一放行，重新执行只需从未完成的元素继续。

注意：当前开源代码里 `xcpt.scala` 的状态机存在，但 `vector-unit.scala` 将 `xcpt.prop.vmu.stall/drain` 与 `top.stall` 硬接为 false，RoCC 译码表中 save/restore/kill 位标注 unused，说明完整的可重启异常路径在开源版中未接通，属于论文描述而非可直接使用的功能。

## 6. 中断

`irq.scala` 定义了向量单元向 Rocket 报告的中断类型（非法指令、访存对齐错误、缺页等）。RoCC 单元把它们汇总为 RoCC `interrupt` 输出。

## 7. 对软件的含义

- 控制线程读取向量结果前必须 `fence`。
- 向量块内部若后续标量 load 依赖前面的向量 store（或反之），用 `vfence`。
- 不同 lane 之间的 store 会被 MOU 串行化，跨 lane 的 store 密集内核会受影响。
- 缺页处理是 OS 层面的功能，裸机测试通常把页表固定映射避免触发。
