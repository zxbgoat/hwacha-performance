# rtl/results：RTL 计时与踪迹日志

**当前基线**（`scripts/check_calibration.py` 用的那几个；改了 RTL 或重新计时后在这里更新）：

| 配置 | 完整基准 | 微基准 | 其他 |
|---|---|---|---|
| 1 lane（`HwachaRocketConfig`） | `rtl-n4096-aligned.log` | `micro-n4096-aligned2.log`、`micro-n4096-pred2.log`（谓词/FMA 分解）、`micro-n4096-pred3.log`（第三轮谓词分解） | `probe4-vru.log`/`probe4-novru.log`（VRU 对照）、`probe5.log`（标量/分支）、`probe6.log`（store 布局）、`probe7.log`（32/64 位探针） |
| 2 lane | `rtl-n4096-l2-fixed.log`（IBoxML 修复后含 gather） | `micro-n4096-l2-aligned.log` | `probe7-l2.log`、`probe8-l2.log`（gather） |
| 2 lane + 2 bank L2 | `rtl-n4096-l2-b2.log` | `micro-n4096-l2-b2.log` | `probe7-l2-b2.log`、`probe8-l2-b2.log`、`tlv-probe7-l2-b2.log`、`tlv-probe8-l2-b2.log`（TileLink 跟踪） |
| 4 lane | `rtl-n4096-l4.log` | `micro-n4096-l4.log` | `rodinia-*-l4.log`、`hcc-n1024-l4.log`（踪迹套件） |
| 8 lane | `rtl-n4096-l8.log` | `micro-n4096-l8.log` | |
| 16 lane（单线程仿真器） | `rtl-n4096-l16.log` | `micro-n4096-l16.log` | `*-l16-threads.log` 是 4 线程仿真器的对照（周期数差 1%–5%） |
| 踪迹套件（1 lane） | `hcc-n1024.log`、`rodinia-*.log` | | Spike 踪迹 `trace-hcc-n1024.log.gz`、`trace-rodinia-*.log.gz` |
| TileLink 通道跟踪 | `tlv-micro-n4096{,-l2,-l4,-l8}.log.gz`、`tlv-bench-n4096.log.gz` | | `scripts/tl_trace_stats.py` 读 |

命名：`<程序>-n<N>[-l<lane>][-b<bank>][-<tag>].log`；`*_warm2` 行是第三次计时（稳态），比较脚本用它。大文件（踪迹、通道跟踪）以 `.log.gz` 存放，脚本与 C++ 模型都能直接读。

`old/`：被后面的运行取代的日志（未对齐数组的第一、二轮结果、N = 16384 与 131072 的中止运行、`.stdout` 原始输出），只为 `docs/24-rtl-calibration.md` §5–§8 的历史表格保留。
