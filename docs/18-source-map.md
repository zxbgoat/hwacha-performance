# 18 开源代码结构与参数速查

仓库：`https://github.com/ucb-bar/hwacha`，Chisel（Scala）实现，依赖 rocket-chip。全部源码在 `src/main/scala/`，约 12k 行。

## 1. 模块层次

```
Hwacha (LazyRoCC)                          hwacha.scala
├── RoCCUnit                               rocc-unit.scala
│   └── CMDQ ×2 (VCMDQ, VRCMDQ)
├── ScalarUnit                             scalar-unit.scala, scalar-decode.scala
│   ├── SRegFile / Scoreboard / MemTracker(4,4)
│   ├── MulDiv (rocket)
│   └── ALU (rocket)
├── ScalarFPUInterface | ScalarFPU         scalar-fpu-interface.scala, scalar-fpu.scala
├── HwachaFrontend (L1 VI$, MiniFrontend)  frontend.scala
├── SMU (LazyModule, TBox, TLB, Table)     smu.scala
├── VRU (LazyModule, 当前未例化)            vru.scala
│   ├── VRURoCCUnit / VRUFrontend
│   ├── RunaheadManager
│   └── PrefetchUnit
├── MasterSequencer                        sequencer-master.scala
├── RPredMaster / RFirstMaster             vfu-rpred.scala, vfu-rfirst.scala
├── MemOrderingUnit                        mou.scala
├── XCPT (状态机)                          xcpt.scala, irq.scala
└── VectorUnit ×nLanes (LazyModule)        vector-unit.scala
    ├── VXU                                vxu.scala
    │   ├── LaneSequencer                  sequencer-lane.scala
    │   ├── Expander                       expander.scala
    │   ├── Lane                           lane.scala
    │   │   ├── LaneCtrl                   lane-ctrl.scala
    │   │   ├── Bank ×4                    bank.scala
    │   │   │   ├── BankRegfile            bank-rf.scala
    │   │   │   ├── ALUSlice ×2            vfu-alu.scala
    │   │   │   └── PLUSlice               vfu-plu.scala
    │   │   ├── IMulSlice ×2               vfu-imul.scala
    │   │   ├── FMASlice ×2×2              vfu-fma.scala
    │   │   ├── FCmpSlice ×2               vfu-fcmp.scala
    │   │   └── FConvSlice ×2              vfu-fconv.scala
    │   └── DecoupledCluster               dcc.scala
    │       ├── VGU / VPU / VSU / VLU      dcc-mem.scala
    │       └── VDU                        dcc-fu.scala
    │           ├── IDivSlice              vfu-idiv.scala
    │           ├── FDivSlice              vfu-fdiv.scala
    │           ├── RPredLane              vfu-rpred.scala
    │           └── RFirstLane             vfu-rfirst.scala
    ├── VMU                                vmu.scala
    │   ├── IBox (IBoxSL | IBoxML), AGU    vmu.scala, vmu-addr.scala
    │   ├── ABox (VVAQ, ABox0, VPAQ, ABox1, ABox2)   vmu-addr.scala
    │   ├── PBox (PBox0, PBox1)            vmu-pred.scala
    │   ├── TBox                           vmu-tlb.scala
    │   ├── SBox                           vmu-sdata.scala
    │   └── MBox (VMT Table)               vmu-memif.scala, vmu-table.scala
    ├── VMUTileLink                        vmu-memif.scala
    ├── MemTracker                         mrt.scala
    └── TLB (rocket)
```

## 2. 文件一览

| 文件 | 行数 | 内容 |
|---|---|---|
| hwacha.scala | 296 | 参数 Field 定义、`UsesHwachaParameters`、顶层 LazyRoCC 与连接 |
| configs.scala | 94 | `DefaultHwachaConfig` |
| TopLevelConfigs.scala | 69 | 示例 SoC 配置 |
| Generator.scala | 53 | 生成入口 |
| consts.scala | 278 | 常量与译码枚举 |
| instructions.scala | 321 | 控制线程与工作线程指令位模式 |
| types-vxu.scala | 608 | VXU 侧 Bundle：IssueOp、SeqEntry、各类 LaneOp |
| types-vmu.scala | 225 | VMU 侧 Bundle |
| util.scala / util-confprec.scala | 192 / 118 | LookAheadCounter、Rotator、打包/速率逻辑 |
| rocc-unit.scala | 481 | RoCC 单元 |
| scalar-unit.scala / scalar-decode.scala | 636 / 418 | 标量单元及译码表 |
| scalar-fpu-interface.scala / scalar-fpu.scala | 200 / 119 | FPU 接口 |
| frontend.scala | 176 | 向量指令缓存前端 |
| smu.scala | 198 | 标量访存单元 |
| vru.scala | 452 | 向量预取单元 |
| sequencer-master.scala / sequencer-lane.scala | 667 / 701 | 序列器 |
| expander.scala | 579 | 展开器 |
| lane.scala / lane-ctrl.scala | 332 / 123 | lane |
| bank.scala / bank-rf.scala | 139 / 217 | bank 与寄存器堆 |
| vfu-*.scala | 35–162 | 功能单元 |
| dcc.scala / dcc-mem.scala / dcc-fu.scala | 124 / 746 / 358 | 解耦簇 |
| vmu.scala / vmu-addr.scala / vmu-pred.scala / vmu-sdata.scala / vmu-memif.scala / vmu-tlb.scala / vmu-table.scala / vmu-util.scala | 353 / 471 / 343 / 101 / 166 / 77 / 46 / 100 | VMU |
| vector-unit.scala / vxu.scala | 77 / 90 | lane 级组装 |
| mou.scala / mrt.scala / xcpt.scala / irq.scala | 79 / 54 / 198 / 80 | 排序、跟踪、异常、中断 |

## 3. 全部配置参数（DefaultHwachaConfig）

| 参数 | 默认 | 说明 |
|---|---|---|
| HwachaIcacheKey | 64 组 × 1 路 × 8 B，8 路 TLB | L1 VI$ |
| HwachaCommitLog | true | 打印提交日志（仿真调试） |
| HwachaNAddressRegs / NScalarRegs / NVectorRegs / NPredRegs | 32 / 64 / 256 / 16 | 架构寄存器数 |
| HwachaRegLen | 64 | 元素最大宽度 |
| HwachaMaxVLen | nBanks × nSRAMRFEntries × bankWidth / regLen = 2048 | 单 lane 最大 HVL |
| HwachaNDTLB / NPTLB | 8 / 4 | TLB |
| HwachaLocalScalarFPU | false（顶层硬编码 false） | 本地标量 FPU |
| HwachaNLanes | 1 | lane 数 |
| HwachaBankWidth / NBanks | 128 / 4 | bank |
| HwachaNSRAMRFEntries / NFFRFEntries / NFFRFReadPorts | 256 / 16 / 3 | SRAM 与触发器寄存器堆 |
| HwachaNPredRFEntries / NPredRFReadPorts | 256 / 3 | 谓词寄存器堆 |
| HwachaNOperandLatches / NPredLatches | 6 / 4 | 全局操作数/谓词锁存器 |
| HwachaWriteSelects | 2 | 写回 mux 选择数 |
| HwachaStagesALU/PLU/IMul/DFMA/SFMA/HFMA/FConv/FCmp | 1/0/3/4/3/3/2/1 | 流水级数 |
| HwachaNSeqEntries | 8 | 序列器槽 |
| HwachaNVVAQEntries / NVPAQEntries / NVSDQEntries / NVLDQEntries | 4 / 24 / 4 / 4 | VMU 队列 |
| HwachaNVMTEntries | 64 | 在途访存表 |
| HwachaNSMUEntries | 16 | SMU 表 |
| HwachaBuildVRU | true（顶层硬编码 false） | VRU |
| HwachaConfPrec | false | 混合精度 |
| HwachaVRUMaxOutstandingPrefetches / VRUEarlyIgnore / VRUMaxRunaheadBytes | 20 / 1 / 16 MiB | VRU 节流 |
| HwachaCMDQLen | 32 | 命令队列 |
| HwachaVSETVLCompress | true | 合并重复 vsetvl |

派生量（`UsesHwachaOnlyParameters`）：`nSlices = 2`、`nStrip = 8`、`maxLStride = 2`、`nvsreq = nvlreq = 512`（MRT 计数器上限，TODO 注释说应按带宽延迟积参数化）。

## 4. 值得注意的硬编码与 TODO

| 位置 | 内容 |
|---|---|
| hwacha.scala | `local_sfpu = false`、`confvru = false`，均注释 TODO（新编码 / TL2 Hint） |
| vector-unit.scala | `xcpt.prop.*.stall/drain` 接 false，异常路径未接通 |
| rocc-unit.scala | save/restore/kill 译码位 unused；`morelax` 恒 false |
| scalar-unit.scala | vfence 不刷新 VI$/TLB；动态舍入模式未从 Rocket 传入 |
| bank-rf.scala / dcc | `selff` 固定 false，FF 寄存器堆未启用 |
| vmu-memif.scala | 未总是请求最小事务尺寸 |
| mrt.scala | 请求计数上限 512 未参数化 |

## 5. 阅读源码的建议顺序

1. `hwacha.scala`（参数、顶层连接）→ `configs.scala`
2. `rocc-unit.scala` → `scalar-unit.scala`（发射路径）
3. `types-vxu.scala`（理解 IssueOp / SeqEntry / LaneOp 的字段）
4. `sequencer-master.scala` → `sequencer-lane.scala` → `expander.scala`
5. `lane.scala` → `bank.scala` → `bank-rf.scala`
6. `dcc*.scala` → `vmu*.scala` → `mrt.scala` → `mou.scala`
7. `util-confprec.scala`（仅在关心混合精度时）
