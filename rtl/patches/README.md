# rtl/patches：复现 RTL 计时所需的外部补丁

这些文件是 `~/hwacha-compiler` 里上游代码树相对基线提交的完整工作区差异（含 hwacha-compiler 仓库自己的修复和本项目为校准加的改动），导出于 2026-09-14。基线：

| 代码树 | 远程 | 基线提交 |
|---|---|---|
| `chipyard` | `https://github.com/ucb-bar/chipyard.git` | `1.11.0`（`ac58f38d`） |
| `chipyard/generators/hwacha` | `https://github.com/ucb-bar/hwacha.git` | `bf799dc`（1.11.0 子模块） |
| `chipyard/generators/rocket-chip-inclusive-cache` | 1.11.0 子模块 | `1332d22` |
| `chipyard/generators/rocket-chip` | 1.11.0 子模块 | `749a3eae9` |
| `esp-isa-sim` | `https://github.com/ucb-bar/esp-isa-sim` | `051d820` |

| 文件 | 应用到 | 内容 |
|---|---|---|
| `esp-isa-sim-base.patch` | `esp-isa-sim` | hwacha-compiler 的 Spike 修复：`insn_t::bits()` 8 字节指令的未定义移位（GCC 13 下所有工作线程指令读到 0）、缺 `<cstdint>` |
| `esp-isa-sim-hwacha-trace.patch` | `esp-isa-sim`（在 base 之后） | 环境变量 `HWACHA_TRACE=<file>` 时输出 `H: WT …`（每条工作线程指令：pc、编码、下一条 pc、vl、活跃掩码）与 `HMEM: …`（每个访存元素的地址），供模型的执行驱动模式使用 |
| `chipyard-hwacha-generator.patch` | `chipyard/generators/hwacha` | hwacha-compiler 的四个集成修复（icache 行宽、frontend 行复用、SMU TLB `prv`、谓词 ALL 归约）与 FPU 类型标签修复、`+hwacha_vmu_trace`；本项目加的 `+hwacha_tl_trace`（VMUTileLink A/D 通道打印）与 `IBoxML` 的 `aret` 修复（`last` 标记，解决 2 lane 以上索引访存的 `qcntr` 断言） |
| `chipyard-inclusivecache-tl-trace.patch` | `chipyard/generators/rocket-chip-inclusive-cache` | `+hwacha_tl_trace` 时打印 L2 bank 内外侧 TileLink 握手与 MSHR 占用 |
| `chipyard-rocketchip-rocc-fpu.patch` | `chipyard/generators/rocket-chip` | hwacha-compiler 的修复：RoCC FPU 端口在仲裁连接后被接成 `DontCare`，Hwacha 标量浮点会挂死 |
| `HwachaLaneConfigs.scala` | 复制到 `chipyard/generators/chipyard/src/main/scala/config/` | `HwachaL2/L4/L8/L16RocketConfig`（`WithNLanes(2/4/8/16) ++ HwachaRocketConfig`）、`HwachaNoVRURocketConfig`（关闭 VRU）、`HwachaL2B2RocketConfig`（2 lane + `WithNBanks(2)`） |

`chipyard-hwacha-generator.patch` 已包含 hwacha-compiler 仓库 `patches/chipyard-hwacha-rtl-fixes.patch` 的内容，二者只应用其一。应用方式见仓库根目录 README 的"复现"一节。
