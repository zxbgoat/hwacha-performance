# 05 微架构总览

本文描述一条向量指令从控制线程发出到退休的完整路径，并解释贯穿整个设计的几个核心机制：strip、systolic bank 执行、chaining、多 lane 交错。各单元的细节见 `modules/` 目录。

## 1. 一条向量指令的生命周期

```
控制线程 vf ──▶ RoCC 单元 ──▶ VCMDQ ──▶ 标量单元取指/译码
                                            │
                    ┌───────────────────────┼──────────────────────┐
                    ▼                       ▼                      ▼
              标量指令本地执行        向量指令 → 主序列器      向量访存 → 各 lane VMU 命令队列
                                            │
                                    ┌───────┴────────┐
                                    ▼                ▼
                              Lane 0 序列器     Lane N 序列器   （非投机乱序发射窗）
                                    │                │
                                    ▼                ▼
                                 展开器           展开器        （序列器操作 → bank µop 移位寄存器）
                                    │                │
                                    ▼                ▼
                       Bank0→Bank1→Bank2→Bank3     …            （systolic 传递，每 bank 一拍）
                                    │
                        ┌───────────┼──────────────┐
                        ▼           ▼              ▼
                   本地 ALU/PLU   共享 VFU0/1/2   BRQ/BPQ → DCC (VGU/VPU/VSU/VLU/VDU) ↔ VMU ↔ L2
                                    │
                                    ▼
                           BWQ → sram-write 回写 VRF
                                    │
                                    ▼
                    所有 lane 完成 → 主序列器释放槽位 → (若为块尾) 通知 VRU 与 MOU
```

分步说明：

1. **RoCC 单元**收到 `vf`，把它同时压入 VCMDQ 和 VRCMDQ。
2. **标量单元**从 VCMDQ 取出 `vf`，从 4 KB L1 VI$ 逐条取 64 位工作线程指令直到 `vstop`。标量整数指令在本地 4 级流水执行；乘除法走解耦的标量乘除单元；标量访存交 SMU；标量浮点通过 FPREQQ/FPRESPQ 使用 Rocket 的 FPU。以上四类都用 scoreboard 互锁目的寄存器。
3. **向量指令**在译码阶段连同标量操作数一起发给**主序列器**和所有 **lane 序列器**。主序列器槽位不足时标量单元停顿。同一条指令可能需要多个序列器操作（例如 AMO 需要 VGU、VCU、VSU、VLU 四个）。
4. **lane 序列器**每周期检查每个槽位的数据冒险、结构冒险和 bank 冒险，用 age 仲裁挑一个就绪操作发给**展开器**。发射粒度是一个 **strip**（8 个 64 位元素，恰好是一次穿过 4 个 bank × 2 个 slice）。
5. **展开器**把序列器操作翻译成一组 bank µop，按功能单元延迟精确放进移位寄存器；每周期移位一格，末端的 µop 送入 bank 0，随后逐拍传向 bank 1、2、3。
6. **bank** 执行 µop：读 SRAM 到操作数锁存器、把锁存器驱动到交叉开关、触发共享功能单元、把结果写回 SRAM，或把操作数/谓词压入 BRQ/BPQ 供解耦簇使用。
7. **变延迟操作**（除法、开方、访存）不预排写回 µop，结果进入 per-bank BWQ 由 VLU 或 VDU 择机写回，序列器通过簿记操作（VIDU、VFDU、VCU、VLU）异步跟踪。
8. 所有 lane 的所有元素完成后，主序列器释放槽位。块内最后一条指令退休时向 VRU 发送完成应答，用于节流。

## 2. strip 与 slice

- 一个 lane 的数据通路 128 位宽，由 4 个 bank 组成；每个 bank 128 位分成 `nSlices = 128/64 = 2` 个 64 位 slice。
- 一个 **strip** = `nBanks × nSlices` = 8 个 64 位元素，对应 µop 穿过全部 bank 一次。
- 序列器的一切进度（vlen 字段递减、eidx 递增、冒险判定）都以 strip 为单位。
- 多 lane 时元素按 **lane stride**（strip 的 1 或 2 倍，运行时可配）交错分到各 lane：lane 0 拿元素 0–7、32–39、…，lane 1 拿 8–15、40–47、…（4 lane、stride=1 strip 时）。

## 3. systolic bank 执行

VRF 由 4 个 256×128 位的 1R1W SRAM 组成。要用单端口 SRAM 喂饱需要 2–3 个操作数的功能单元，方法是：

- 每个 bank 带若干**操作数锁存器**（默认 6 个）和**谓词锁存器**（默认 4 个）。
- µop 序列 `sram-read → opl → xbar → fop → sram-write` 逐拍进入 bank 0，然后依次进入 bank 1、2、3。第 k 拍 bank 0 读第 k 个操作数时，bank 1 正在读第 k−1 个。
- 结果：经过 n 个 bank 的启动延迟后，每周期能向共享功能单元提供 n 个操作数，整个调度**永不停顿**。
- 展开器把写回 µop 放在恰好等于功能单元流水延迟的位置，所以 FMA（4 级）的 sram-write 在 fop 之后第 4 拍到达同一 bank。

论文图 8.7 用 4 bank、2 操作数举例：周期 0 bank0 读 R，周期 1 bank0 读 R 同时 bank1 读上一操作的 R，周期 2 bank0 触发 xbar+fop，周期 3 bank0 写回 W，周期 4、5 bank1、bank2 写回，同时新的读已经进入流水。

## 4. 冒险与 chaining

- 主序列器每个槽位记录 base 寄存器号和 RAW/WAR/WAW 冒险位图（相对于已发射的更老操作）。
- lane 序列器把 base 寄存器号映射为物理行地址，跟踪剩余 vlen、当前 eidx 与 age。
- WAR 检查依赖 `nRPorts <= 3` 的约束（源码 `require`），否则要对照 SRAM 读 ticker 检查。
- **chaining 是自然涌现的**：lane 调度器只要清除冒险后连续发射两个相关操作，展开器就把它们的 µop 交错放进移位寄存器，后一条的读会恰好跟在前一条对应 strip 的写之后。不需要传统向量机中专门的 chaining 逻辑。
- 混合精度下不同速率的操作 chaining 需要在展开器移位寄存器中做区间重叠检查，见 `16-mixed-precision.md`。

## 5. 功能单元布局

| 位置 | 单元 | 说明 |
|---|---|---|
| 每 bank 私有 | ALU、PLU | 直接连在 bank 读写口上，整数/谓词操作不经过交叉开关，无结构冒险 |
| 共享簇 VFU0 | FMA0、IMul、FConv | 通过操作数交叉开关供操作数，簇内共享操作数线、谓词线和结果线 |
| 共享簇 VFU1 | FMA1、FCmp | 第二个 FMA 使双 FMA 并行成为可能 |
| 共享簇 VFU2 | FDiv/FSqrt、IDiv、Reduce | 变延迟，通过 LRQ/LPQ 解耦 |
| 解耦簇 DCC | VGU、VPU、VSU、VLU、VDU | 连接 bank 队列与 VMU / 变延迟单元 |

每个 FMA 单元支持 2×64 位、4×32 位或 8×16 位（混合精度启用后），因此单 lane 峰值为每周期 4 个双精度 FMA。

## 6. 多 lane

- lane 数必须是 2 的幂；主序列器同步发射，各 lane 独立执行。
- 元素交错分配（见第 2 节）使负载均衡，也让 VMU 可以复用地址生成通路作为短迭代乘法器计算任意常量步长的基址。
- 代价：单位步长访存在每个 lane 看来有"空洞"，VMU issue 单元把向量拆成连续段处理；若基址未对齐到 128 位，相邻 lane 会重复请求同一 TileLink beat（论文图 8.12）。
- 归约（一致性分支、vfirst）需要主序列器同步全部 lane，再把结果送回标量单元。

## 7. 与 45nm 芯片版本的差异

ESSCIRC 2014 的 EOS 芯片对应 Hwacha 早期版本：8 个 64 位 bank（而非 4 个 128 位）、向量访存指令留在标量流中、FPU 与 Rocket 共享、单 lane。第四代把 bank 加宽到 128 位使功能单元吞吐翻倍，并加入了完整谓词、一致性分支、归约和硬件除法/开方。
