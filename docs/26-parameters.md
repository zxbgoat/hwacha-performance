# 26. 模型参数表

C++ 模型（`hwacha-perf/`）的全部参数，按来源分三类：**结构**（对应 Chisel 的配置项，改它等于改设计）、**校准常数**（从 RTL 测得的经验值，只在 `configs/rtl-hwacha-rocket*.json` 里有意义，改设计时不该动）、**描述开关**（选择建模什么，不是硬件参数）。默认值在 `hwacha-perf/src/hwacha.hh`（`HwachaParams`）与 `hwacha-perf/src/main.cc`（存储系统），`--set key=val` / `--set mem.key=val` 覆盖，`configs/*.json` 的 `hwacha` / `memory` 段同名。

## 1. Hwacha 结构参数（`hwacha` 段）

| 键 | 默认 | 对应 Chisel（`docs/18-source-map.md`） | 说明 |
|---|---|---|---|
| `n_lanes` | 1 | `HwachaNLanes` | lane 数 |
| `n_banks` / `bank_width` / `reg_len` | 4 / 128 / 64 | `HwachaNBanks` / `HwachaBankWidth` / `HwachaRegLen` | 每 lane 的 VRF bank 数与宽度；nSlices = bank_width / reg_len = 2，nStrip = 8 |
| `n_sram_entries` / `n_pred_entries` | 256 / 256 | `HwachaNSRAMRFEntries` / `HwachaNPredRFEntries` | 每 bank 的向量/谓词寄存器堆条目 |
| `n_vector_regs` / `n_pred_regs` | 256 / 16 | | 架构寄存器数（决定 HVL） |
| `n_seq_entries` | 8 | `HwachaNSeqEntries` | 序列器条目数 |
| `n_fma_units` | 2 | | 每 lane 的 FMA 簇数（见 §10.14：同一块内的独立 FMA 在 RTL 里串行） |
| `stages_alu/plu/imul/dfma/sfma/hfma/fconv/fcmp` | 1/0/3/4/3/3/2/1 | `HwachaStages*` | 各功能单元流水级数 |
| `n_vmt_entries` | 64 | `HwachaNVMTEntries` | 每 lane 在途访存请求上限 |
| `vsdq_beats` / `vldq_beats` / `vvaq_entries` | 8 / 4 / 4 | `HwachaNVSDQ/VLDQ/VVAQ` | VMU 队列深度（VSDQ 按 16 B 数据量计条目） |
| `brq_depth` / `bwq_depth` | 2 / 2 | | bank 读/写队列 |
| `tl_data_bytes` | 16 | `SystemBusWidth` 128 | 每 beat 字节数 |
| `build_vru` / `vru_max_outstanding` / `vru_early_ignore` / `vru_max_runahead_bytes` | true / 20 / 1 / 16 MB | `HwachaBuildVRU`、`HwachaVRU*` | VRU（开源 RTL 的 SimDRAM 下无法校验，§10.13） |
| `conf_prec` | false | `HwachaConfPrec` | 混合精度（论文配置；未校准） |
| `cmdq_len` | 32 | | RoCC 命令队列 |
| `tlb_entries` / `page_bytes` | 8 / 4096 | `HwachaNDTLB` | VMU TLB |
| `freq_ghz` | 1.0 | | 只影响 GFLOPS 报告 |

## 2. Hwacha 校准常数（`hwacha` 段）

| 键 | RTL 配置值 | 出处（`24-rtl-calibration.md`） | 含义 |
|---|---|---|---|
| `vf_fetch_latency` | 2 | §3 | vf 命令到第一条指令发射 |
| `vf_block_overhead` | 0 | | 每个 vf 块的固定附加周期（保留） |
| `vf_lane_sync_cycles` | 20（多 lane） | §10.9 | 多 lane 每块的固定附加周期（micro_empty） |
| `scalar_smu_latency` / `scalar_fpu_latency` / `scalar_muldiv_latency` | 12 / 8 / 5 | §8.2 | 块内标量 load / 浮点 / 乘除延迟 |
| `branch_strip_cycles` / `branch_resolve_latency` | 6 / 0 | §8.2 | 一致性分支的谓词归约每 strip 拍数 / 解析延迟 |
| `fdiv_cycles_per_elem` / `fsqrt_cycles_per_elem` / `idiv_cycles_per_elem` | 3 / 5 / 65 | §10.7 | 变延迟单元每 lane 每元素拍数（idiv 未校准） |
| `vmu_issue_latency` / `vlu_latency` | 4 / 3 | §3 | VMU 发射延迟 / VLU 写回延迟 |
| `store_beat_cycles` / `load_beat_cycles` | 1.0 / 1.0 | §10.9 | lane 端口每 beat 占用（store 的附加代价已移到 L2 侧） |
| `seq_age_rule` / `plu_port` | true / true | §10.12 | 序列器 age 两级优先级；谓词逻辑独立发射口 |
| `plu_occupancy` | 0 | §10.12 | 谓词逻辑单元每 strip 占用（实验用，设非 0 会让 divloop 变差） |
| `pred_port_cycles` / `pred_port_int_cycles` | 2 / 0 | §10.14 | 共享谓词端口：vcmp 写与浮点谓词化读 / 整数谓词化读各占的拍数 |
| `lane_max_lead_beats` | 1（多 lane） | §10.14 | 同一访存指令上 lane 间最大领先 beat 数（锁步） |
| `ibox_lane_elem_cycles` | 1（不限） | §10.15 | 多 lane 时每条 lane 每隔几拍发一个索引访存元素请求（实验开关） |
| `lockstep_indexed` | false | §10.15 | 索引访存是否也受 lane 锁步约束 |
| `branch_pred_port_cycles` | 0 | §10.15 | 一致性分支读谓词占谓词端口的拍数（实验开关） |
| `shared_line_store_turnaround` | 1.5（多 lane） | §10.14 | 多 lane 多 bank、strip 不足一行时 store 每换行的停顿 |
| `ctrl_cycles_per_iter` | 0 | | 控制线程每次 stripmine 的簿记周期（静态模式；`@ctrl` 注解） |

## 3. 存储系统结构参数（`memory` 段）

| 键 | 默认 | 说明 |
|---|---|---|
| `l2_banks` / `l2_bytes_per_bank` / `l2_ways` / `line_bytes` | 4 / 256 KB / 8 / 64 | L2 组织；RTL 配置 1 bank × 512 KB（`WithNBanks`、`InclusiveCache`） |
| `l2_hit_latency`（= `l2_tag_latency` + `l2_data_latency`） | 24（RTL：A→D 4 拍在 lane 侧另有 VMU 延迟） | 命中延迟 |
| `l2_trackers_per_bank` | 16 | 每 bank MSHR 数 |
| `l2_supports_amo` | true | L2 原子操作 |
| `dram_channels`、`dram_tck_ps`、`dram_banks`、`dram_row_bytes`、`dram_burst_bytes`、`dram_t*`、`dram_frontend/backend_latency` | LPDDR3-1866 ×2 | JEDEC 时序（`23-cpp-model.md`）；RTL 配置设为极快以对应 SimDRAM（§8.3） |
| `dram_latency` / `dram_bytes_per_cycle_per_channel` | 110 / 3.73 | 早期简化模型的键，C++ 只在 JEDEC 参数缺省时参考 |
| `rocc_shared_port` / `rocc_switch_penalty` | false / 0 | 所有 lane 经 RoCC 单端口进系统总线的近似（§10.1 后关闭：单 bank 已足够） |
| `tlb_entries` / `tlb_miss_latency` | 8 / 40 | VMU TLB（RTL 裸机 M 态无翻译，配置为 0） |

## 4. 存储系统校准常数（`memory` 段）

| 键 | RTL 配置值 | 出处 | 含义 |
|---|---|---|---|
| `l2_store_beat_cycles` | 1.0 | §10.9 | store 通路每 beat 基础占用 |
| `l2_store_switch` / `l2_partial_store_switch` | 0 / 0.2 | §10.9、§10.7 | 换行 / 部分写（跨步、索引 store）换行的附加拍数 |
| `l2_store_conflict` | `1:0.05,2:0.09,4:0.20,8:0.75,16:0.97` | §10.10 | 按并发行数查表的 store 附加拍数（读-改-写冲突），log2 插值 |
| `l2_store_window` | 32 | §10.10 | 并发行数的观察窗口（beat 数） |
| `l2_store_conflict_global` | true | §10.15 | 并发行数跨所有 bank 共同计数（每 bank 一条 lane 时 4 lane 4 bank 的 store 与 4 lane 1 bank 一样贵） |
| `l2_store_blocks_loads` | true | §10.12 | A 通道按序：store 通路忙时 load 也等 |
| `l1d_probe_cycles` / `l1d_dirty_bytes` | 4 / 16384 | §10.12 | 冷启动描述：L1D 脏行探测每行拍数 / 脏行字节数 |

## 5. 描述开关

| 键 | 含义 |
|---|---|
| `mem.warm_l2` | 内核数组（或踪迹触及的行）预先驻留 L2——对应 RTL 的稳态计时 |
| `mem.cold_start` | 打开 L1D 脏行探测——对应 RTL 的第一次计时（`compare_rtl.py --cold`） |
| `commit_log` | 每条指令发射/完成的日志 |
| `--trace` / `--trace-range` / `--trace-blocks` / `--trace-base` | 执行驱动模式（§8.1、§10.4） |

## 6. 哪些能动

- 做设计空间研究（`25-design-space.md`）：改第 1、3 类，保持第 2、4 类不变。校准常数是在 `HwachaRocketConfig` 的微架构上测得的，改了 lane 内结构（bank 数、序列器深度）它们不一定还成立——例如 `l2_store_conflict` 是按单 L2 bank 拟合的，多 bank 下偏乐观 5%–10%（§10.14）。
- 校准新的 RTL：先跑 `rtl/` 的微基准与探针，按 `24-rtl-calibration.md` 各节的顺序确定第 2、4 类的值，再用 `scripts/check_calibration.py --update` 记基线。
