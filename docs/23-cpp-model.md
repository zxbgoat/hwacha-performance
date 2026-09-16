# 23 C++ 事件驱动周期级模型（hwacha-sim）

代码位于 `hwacha-perf/`。它参照 gem5 的组织方式重写了性能模型：事件队列驱动的模拟内核、SimObject/ClockedObject 对象模型、Port/Packet 时序协议与 retry 反压、逐拍仲裁的交叉开关与 L2 bank、带 JEDEC 时序约束与 FR-FCFS 调度的 DRAM 控制器。Hwacha 内部流水以每周期一个 tick 事件推进（与 gem5 CPU 模型相同），访存则完全走事件驱动的存储系统。输入格式（`kernels/*.S`）与参数文件（`configs/*.json`）与 Python 模型共用。

## 1. 与 gem5 的对应关系

| gem5 | hwacha-sim（`hwacha-perf/src/`） |
|---|---|
| `EventQueue` / `Event` / `EventFunctionWrapper`，tick = 1 ps | `sim::EventQueue`、`sim::Event`、`sim::EventFunctionWrapper`，tick = 1 ps；按 (when, priority, seq) 排序，惰性 deschedule |
| `SimObject`、`ClockedObject`（时钟域、`clockEdge`、`curCycle`） | `sim::SimObject`、`sim::ClockedObject`；核心 1 GHz，DRAM 用自己的 tCK |
| `Stats::Scalar` / `Vector`，`regStats()` | `sim::stats::Scalar` / `Vector`，`Registry::dump/dumpJson` |
| Python 参数系统 | `sim::Json` + `sim::Params`（读取 `configs/*.json`，`--set key=val` 覆盖） |
| `Packet`、`RequestPort`/`ResponsePort`、`sendTimingReq`/`recvTimingResp`/`sendRetry` | `mem::Packet`（带 SenderState 栈）、`mem::RequestPort`/`ResponsePort`，同一套 timing 协议：拒收返回 false，稍后 retry |
| `NoncoherentXBar` 的 Layer | `mem::Xbar`：每个目的端口一条 layer，IDLE/BUSY/RETRY 三态，繁忙时对源端口 retry，占用 = 包大小 / 位宽 |
| classic `Cache`（MSHR、target、写分配、写回） | `mem::L2Bank`：每 bank 组相联 LRU、MSHR + target 合并、每拍一次 tag 访问、写分配、脏行写回、预取目标 |
| `DRAMCtrl`（Hansson 2014） | `mem::DRAMCtrl`：读/写队列与写排空阈值、FR-FCFS（选可最早发列命令者、行命中优先）、bank/rank 级 tRCD/tRP/tRAS/tRC/tCCD/tRTP/tWR/tWTR/tRTW/tRRD/tFAW、tREFI/tRFC 刷新、前后端延迟 |
| CPU `tickEvent` | `hw::Hwacha::tick()`：每周期依次推进 VLU 写回、访存完成、退休、VMU 发射、lane 调度、标量单元、控制线程、VRU |

## 2. 相比 Python 模型提高的精度

| 部分 | Python 模型 | C++ 模型 |
|---|---|---|
| 时间推进 | 单周期循环，存储侧用"下一空闲时刻"解析队列 | 事件驱动；存储侧每个请求经端口协议逐拍仲裁，拒收/重试真实发生 |
| L2 | 命中/缺失在请求时刻结算，bank 只有"下一空闲时刻" | 每 bank 每拍一次 tag 访问，MSHR 数量与 target 合并，MSHR 满/tag 忙时反压到交叉开关 |
| DRAM | 每通道固定延迟 + 带宽计数 | 命令级时序：ACT/PRE/RD/WR 受 JEDEC 约束，行命中/缺失、tFAW、刷新、读写切换 |
| VMU ↔ 存储 | 请求即得完成时刻 | 每 beat 一个 Packet，在途上限（VMT）、VLDQ 深度反压响应、VSDQ 容量限制 VSU 领先量、端口忙时 retry |
| VRF 写口 | 每 lane 一个写口 | 每 bank 一个写口，展开器写 µop 在 `w + bank` 到达各 bank；VLU 的 BWQ 写按 bank 仲裁（展开器优先） |
| load 写回 | strip 全部返回后一次占用写口 | 每 beat 进 VLDQ → 经 `vlu_latency` 进对应 bank 的 BWQ（深度 2）→ 逐 bank 写；跨 bank 的 beat 需全部 bank 写完 |
| load 冒险 | 写回时门控 | 在 VMU 发请求前按 VCU 语义门控 WAW/WAR，避免 VLDQ 队头阻塞（对应硬件序列器在 VCU 处的检查） |
| VRU 预取 | 立即得到到达时刻 | 真实的 PrefetchReq 经交叉开关到 L2，作为 MSHR 目标；应答按 1 拍占用，在途数由应答计数 |
| 速度 | 约 8 万周期/秒 | 约 140 万周期/秒（daxpy，1 lane） |

保持与 Python 相同的抽象：strip（8 个 64 位元素）粒度的序列器与冒险检查、资源时间线表示的展开器 ticker、每 lane 一个调度端口、控制线程按命令计数建模、不做功能执行。

## 3. 构建与使用

```bash
cd hwacha-perf && mkdir -p build && cd build && cmake -G Ninja .. && ninja
ctest --output-on-failure            # memtest（存储系统）+ kernels（全部内核与关键性质）
./hwacha-sim run ../../kernels/daxpy.S --n 65536
./hwacha-sim run ../../kernels/dgemm_opt.S --lanes 4 --n 131072 --json
./hwacha-sim run ../../kernels/vvadd.S --config ../../configs/open-source.json --stats
./hwacha-sim run ../../kernels/saxpy.S --set conf_prec=true --set n_seq_entries=16 \
    --set mem.dram_tRCD=20 --set mem.l2_trackers_per_bank=32
```

`--trace <spike-trace> [--trace-range lo:hi] [--trace-blocks a:b] [--trace-base pc]` 切换到执行驱动（见 `docs/24-rtl-calibration.md` 8.1 节；`--trace-base` 用于 hwacha-cc 生成的多入口 vf 块，见 10.4 节）。`--stats` 输出所有 SimObject 注册的统计（交叉开关、每个 L2 bank、每个 DRAM 通道），`--json` 输出与 Python 模型同名的汇总字段，便于脚本比较。

存储系统参数键名与 Python 的 `MemoryConfig` 一致（`l2_banks`、`l2_bytes_per_bank`、`l2_ways`、`l2_hit_latency`、`l2_trackers_per_bank`、`dram_channels`、`tlb_entries`、`tlb_miss_latency`、`l2_supports_amo`），另加 DRAM 时序键：`dram_tck_ps`（默认 1072，LPDDR3-1866）、`dram_banks`、`dram_row_bytes`、`dram_burst_bytes`、`dram_tRCD`、`dram_tRP`、`dram_tRAS`、`dram_tRC`、`dram_tCL`、`dram_tCWL`、`dram_tBURST`、`dram_tCCD`、`dram_tRTP`、`dram_tWR`、`dram_tWTR`、`dram_tRTW`、`dram_tRRD`、`dram_tFAW`、`dram_tREFI`、`dram_tRFC`、`dram_frontend_latency`、`dram_backend_latency`、`dram_read_queue`、`dram_write_queue`（单位 tCK）。Python 模型的 `dram_latency` / `dram_bytes_per_cycle_per_channel` 在 C++ 中不再使用，带宽由 tCK 与 burst 推出（LPDDR3-1866 ×32 每通道约 7.46 B/ns）。

## 4. 与（已删除的）Python 模型的对比（历史记录，v0.0.1 时）

两个模型在相同参数（Python 侧把每通道 DRAM 带宽设为 7.46 B/周期以匹配 LPDDR3-1866）、n = 16384、单 lane、VRU 开启下的结果：

| kernel | Python cycles | C++ cycles | 差异 | GFLOPS (Py / C++) | FMA util (Py / C++) |
|---|---|---|---|---|---|
| vvadd | 28533 | 29528 | +3.5% | 1.15 / 1.11 | 14.4% / 13.9% |
| saxpy | 14801 | 14877 | +0.5% | 2.21 / 2.20 | 27.7% / 27.5% |
| daxpy | 28533 | 29528 | +3.5% | 1.15 / 1.11 | 14.4% / 13.9% |
| csaxpy | 17126 | 17764 | +3.7% | 1.91 / 1.84 | 23.9% / 23.1% |
| dgemm_opt | 33122 | 33011 | −0.3% | 7.91 / 7.94 | 98.9% / 99.3% |
| sfilter | 23487 | 24173 | +2.9% | 4.19 / 4.07 | 52.3% / 50.8% |
| gather | 46110 | 47858 | +3.8% | – | – |
| fma_peak | 32269 | 32270 | 0.0% | 8.00 / 8.00 | 100% / 100% |

计算受限内核两者一致；访存受限内核 C++ 模型慢 3%–4%，来自逐拍仲裁、MSHR/tag 反压和 DRAM 行缺失/刷新等 Python 模型忽略的效应。

多 lane 扩展（dgemm_opt，n = 131072）：

| lanes | cycles | GFLOPS | FMA util |
|---|---|---|---|
| 1 | 262387 | 7.99 | 99.9% |
| 2 | 133101 | 15.76 | 98.5% |
| 4 | 72935 | 28.75 | 89.9% |

独立的存储系统测试（`tests/memtest.cc`）：顺序流读取 8 MB 得到 14.36 B/周期（LPDDR3-1866 双通道峰值 14.9 的 96%），L2 驻留读取 15.96 B/周期（受 16 B/周期端口限制），随机 16 B 读取 2.63 B/周期（受 tFAW/tRRD 与 tRC 限制）。

## 5. 死锁检测

`Hwacha::tick()` 跟踪进展（发射的指令与 strip、发出与返回的 beat）；连续 200 000 周期无进展时打印每个 lane 的在途操作状态（strip 指针、已发送/已返回/已写回 beat 数、VLDQ/VSDQ 占用、端口阻塞）并退出。开发过程中它定位了 VLDQ 的队头阻塞死锁，最终按硬件 VCU 语义在 VMU 发请求前做 load 的 WAW/WAR 门控解决。

## 6. 仍然是"周期近似"的原因

- 序列器与展开器仍以 strip 为粒度、以资源时间线表示，未逐拍模拟 bank 内的 µop 状态机；每 lane 每周期只有一个调度端口。
- VMU 内地址流水（IBox/ABox0-2/PBox）折叠为固定的发射延迟 + 每拍一个 beat；未建模谓词跳过（density-time skipping）与 VPAQ 深度。
- L2 一致性协议对 L1D 的探测、L2 内 AMO ALU 占用、TileLink 消息级细节未建模。
- DRAM 控制器把一条 64 B 请求当作同一行的连续 burst，未细分 bank group；地址映射固定为 RoRaBaCo。
- 控制线程与标量单元不执行功能语义，分支结果与索引地址来自注解或伪随机数。
- 参数是文献校准值，未用 RTL 逐周期对拍。

因此它的定位仍然是比较与趋势分析，只是相对 Python 模型把存储系统与数据搬运路径推进到了逐拍仲裁的精度；要成为严格意义上的周期精确模型，需要用 Verilator 运行 `ucb-bar/hwacha` 的 RTL 在同一批内核上校准上述各项。

## 附：RTL 校准后新增的参数

- `mem.l2_store_beat_cycles` / `mem.l2_store_switch` / `mem.l2_partial_store_switch` / `mem.l2_store_conflict` / `mem.l2_store_window`：L2 bank 的 store 通路占用（`24-rtl-calibration.md` 10.9、10.10 节），所有 lane 共享；`l2_store_conflict` 是按并发行数查表的附加代价（`mem.cc` `L2Bank::storeBeatCost`）。 `mem.l2_store_conflict_global`（默认开）：并发行数在所有 bank 间共同计数（24 节 10.15）。`store_beat_cycles` 仍是 lane 端口侧的附加代价，RTL 配置里为 1.0。
- `seq_age_rule`（默认开）：RTL 序列器的 age 两级优先级——刚发过 strip 的条目在 nBanks 拍内让位给其他就绪条目，没有别的就绪条目时仍可发射；store 与索引访存只从各自最老的条目发射。`plu_port`（默认开）：vpop 等谓词逻辑走独立的 VIPU 发射口。`plu_occupancy`（默认 0）：谓词逻辑单元每 strip 占用，实验用。
- `vf_lane_sync_cycles`：多 lane 时每个 vf 块的固定附加开销（RTL 配置 20）。
- `pred_port_cycles`（RTL 配置 2）/ `pred_port_int_cycles`（0）：共享谓词端口——vcmp 类写谓词与浮点类谓词化读各占的拍数/strip（24 节 10.14）。
- `branch_pred_port_cycles`（0）：一致性分支读谓词占共享谓词端口的拍数，实验用（24 节 10.15：设 1 修 pcmp_br/pgain 但 divloop 变 +29%）。`ibox_lane_elem_cycles`（默认 1 = 不限）：多 lane 时每条 lane 每隔几拍才能发一个索引访存元素请求，实验开关；`lockstep_indexed`（默认关）：索引访存是否也受 lane 锁步约束（24 节 10.15）。
- `lane_max_lead_beats`（多 lane 配置 1）：同一条访存指令上任一 lane 最多比最慢的 lane 多发的 beat 数（lane 锁步）；`shared_line_store_turnaround`（1.5）：多 lane 多 bank 下一个 strip 不足一行的 store 每换一行的 VMU 停顿。VMU 每拍只发一个请求。
- `mem.cold_start` / `mem.l1d_probe_cycles`（4）/ `mem.l1d_dirty_bytes`（16384）：冷启动描述——按内核数组列表顺序取最后 16 KB 当作标量核刚写、仍在 L1D 里的脏行，第一次向量访问它们时 L2 要探测 L1D（每行多 4 拍并占住 bank）。`compare_rtl.py --cold` 用它与 RTL 的第一次计时比较（24 节 10.12）。
- `fsqrt_cycles_per_elem`：与 `fdiv_cycles_per_elem` 分开的开方吞吐（10.7 节）。
- `--trace-base`：多入口 vf 块的踪迹映射（10.4 节）。
- 跨步/索引访存每元素一个请求、VSDQ 按 16 B 数据量计条目（10.7 节）。
