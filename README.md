# hwacha-performance

UC Berkeley Hwacha 解耦向量取指加速器的文档集与性能模型。

- `docs/`：设计思想、编程模型、ISA、整体架构、微架构与各子模块文档（从 `docs/README.md` 进入）
- `hwacha-perf/`：C++ 的 gem5 风格事件驱动周期级模型（事件队列、Port/Packet、逐拍仲裁的 L2、JEDEC 时序的 DRAM 控制器；抽象层次与内核文件格式见 `docs/22-performance-model.md`，实现见 `docs/23-cpp-model.md`）。早期的 Python 模型已在 v0.0.7 删除
- `kernels/`：用 Hwacha 汇编写的示例内核（vvadd、saxpy、daxpy、csaxpy、dgemm 分块、模板滤波、gather、FMA 峰值）
- `configs/`：论文评估配置、开源主线配置、混合精度配置、理想内存配置、RTL 校准配置
- `rtl/`：在 Chipyard 1.11 `HwachaRocketConfig`（1 lane）、`HwachaL2RocketConfig`（2 lane）、`HwachaL4/L8/L16RocketConfig`（4/8/16 lane）的 Verilator RTL 上运行同一批内核的基准、微基准、探针与 Rodinia 程序；`rtl/results/` 保存了全部 RTL 计时与踪迹日志，`rtl/patches/` 是复现所需的上游补丁；`make calibrate` 一键回归（说明见 `docs/24-rtl-calibration.md`）
- `kernels/hcc/`：hwacha-cc 编译得到的 OpenCL 内核向量块（含分歧循环），用于执行驱动模式的校验
- `scripts/`：与 RTL 比较、踪迹处理、设计空间研究、批量汇总等脚本

## 快速开始

```bash
cd hwacha-perf && mkdir -p build && cd build && cmake -G Ninja .. && ninja && ctest && cd ../..
S=hwacha-perf/build/hwacha-sim
$S run kernels/daxpy.S --n 65536                          # 论文配置（默认）
$S run kernels/dgemm_opt.S --lanes 4 --n 131072 --json
$S run kernels/saxpy.S --config configs/paper-28nm-mxp.json
$S run kernels/vvadd.S --set n_seq_entries=16 --set mem.dram_latency=80 --stats
# 执行驱动：用带补丁的 Spike 踪迹提供分支结果、活跃掩码与索引地址
python3 scripts/hwacha_trace.py run rtl/bench-n4096.riscv -o trace.log
$S run kernels/csaxpy.S --config configs/rtl-hwacha-rocket.json --trace trace.log --trace-range $(python3 scripts/hwacha_trace.py range rtl/bench-n4096.riscv csaxpy_vf)
python3 scripts/summary.py --n 16384 --lanes 1,2,4        # 批量汇总表
python3 scripts/design_space.py                           # docs/25 的设计空间研究
```

## 内核文件格式

```
# @kernel saxpy
# @n 65536
# @cfg v64=0 v32=2 vp=0          ← vsetcfg 寄存器用量，决定 HVL
# @array x elem=4 n=65536
# @array y elem=4 n=65536
# @va va0 = x                    ← 指针，每次 stripmine 前进 vl×elem
# @va va1 = y
saxpy_vf:
    vlw       vv0, va0
    vlw       vv1, va1
    vfmadd.s  vv1, vv0, vs1, vv1
    vsw       vv1, va1
    vstop
```

更多语法（常量步长、偏移、索引访存注解、一致性分支循环次数、混合精度）见 `docs/22-performance-model.md`。

## 在新机器上复现

分三层，后一层依赖前一层。所有已经测得的 RTL 日志都在 `rtl/results/` 里并随仓库提交，所以第一层不需要任何 RISC-V 工具链或 Verilator 就能复现文档里的全部模型误差表；只有想重新计时或改 RTL 时才需要第二、三层。

### 第一层：只跑模型（几分钟）

需要 CMake ≥ 3.16、Ninja、支持 C++17 的编译器，以及 Python 3（只用于脚本，没有第三方依赖）。

```bash
git clone <本仓库> hwacha-performance && cd hwacha-performance
cd hwacha-perf && mkdir -p build && cd build && cmake -G Ninja .. && ninja && ctest --output-on-failure && cd ../..
```

`ctest` 里的 3 个测试：`memtest`/`kernels`（模型自检）、`calibrate`（`scripts/check_calibration.py`：1/2/4/8/16 lane 的基准与微基准、hwacha-cc 与 Rodinia 内核共 122 个条目，每个条目的 RTL 误差不超过 20%，且相对 `hwacha-perf/tests/calibration_baseline.json` 里记录的模型周期数漂移不超过 2%；改模型后确认漂移合理再 `--update` 基线）。单独看表用比较脚本：

```bash
python3 scripts/compare_rtl.py --logs rtl/results/rtl-n4096-aligned.log,rtl/results/micro-n4096-aligned2.log
python3 scripts/compare_rtl.py --config configs/rtl-hwacha-rocket-l2.json --logs rtl/results/rtl-n4096-l2-fixed.log,rtl/results/micro-n4096-l2-aligned.log
python3 scripts/compare_rtl.py --config configs/rtl-hwacha-rocket-l4.json --logs rtl/results/rtl-n4096-l4.log,rtl/results/micro-n4096-l4.log
python3 scripts/compare_rodinia.py            # Rodinia 内核；--suite hcc 是 hwacha-cc bench 内核；--lanes 4 用 4 lane 的日志与配置
python3 scripts/compare_rtl.py --cold --logs rtl/results/rtl-n4096-aligned.log   # 与第一次（冷）计时比较，模型打开冷启动描述
python3 scripts/tl_trace_stats.py rtl/results/tlv-micro-n4096-l4.log   # TileLink 通道级跟踪的统计（docs/24 第 10.1/10.9 节；大日志以 .log.gz 存放，脚本与模型都能直接读，给 .log 路径会自动找 .gz）
python3 scripts/design_space.py > /tmp/design_space.md                  # docs/25-design-space.md 的全部表格（约 5 分钟）
```

预期结果（C++ 模型，`docs/24-rtl-calibration.md` 10.10 节）：完整基准平均误差 1 lane 2.5%、2 lane 2.4%、4 lane 1.8%、8 lane 2.5%、16 lane 1.3%；Rodinia 平均 4.2%（最大 9.2%）。`compare_rodinia.py` 默认用 `riscv64-unknown-elf-nm` 读 `rtl/rodinia/<prog>.riscv` 的符号地址来切分踪迹；没有工具链时用 `--syms` 直接给出（五个内核在当前二进制里的地址范围都相同）：

```bash
python3 scripts/compare_rodinia.py --syms nn=80002010:800020a0,kmeans_swap=800021a0:800022a0,kmeans_c=80002010:800021a0,pgain=80002030:800022e0,pathfinder=80002010:800024c0
```

### 第二层：Spike 踪迹与 RISC-V 二进制（约 1 小时，主要是安装）

`rtl/` 与 `scripts/hwacha_trace.py` 假定上游代码树按 [hwacha-compiler](https://github.com/zxbgoat/hwacha-compiler) 仓库的布局放在 `~/hwacha-compiler`（可用环境变量 `HWACHA_ROOT` 或 `make HWROOT=...` 改）：

```
~/hwacha-compiler/
├── esp-isa-sim/            git clone https://github.com/ucb-bar/esp-isa-sim（基线 051d820）
├── esp-tests/              git clone https://github.com/ucb-bar/esp-tests（只用 benchmarks/common 与 env）
├── chipyard/               git clone -b 1.11.0 https://github.com/ucb-bar/chipyard（第三层才需要；.conda-env/esp-tools 里的 GCC 9.2 + 支持 -march=rv64gcxhwacha 的 binutils 第二层就要用）
├── hwacha-cc/test/apps/    hwacha-compiler 仓库里 Rodinia 内核的 .s（rtl/rodinia 直接引用）与 bench.s（kernels/hcc）
├── install/  install-hlog/ 两个 Spike：普通 / 带 H: 提交日志与 HWACHA_TRACE 踪迹
└── build/
```

按 hwacha-compiler 的 README 装好 conda、`esp-tools`（`chipyard/build-setup.sh esp-tools --use-lean-conda --skip-toolchain ...`）之后，打本仓库 `rtl/patches/` 里的 Spike 补丁并构建两个 Spike：

```bash
cd ~/hwacha-compiler
git -C esp-isa-sim apply $PERF/rtl/patches/esp-isa-sim-base.patch             # $PERF = 本仓库路径
mkdir -p build/spike && (cd build/spike && ../../esp-isa-sim/configure --prefix=$PWD/../../install && make -j8 && make install)
git -C esp-isa-sim apply $PERF/rtl/patches/esp-isa-sim-hwacha-trace.patch
mkdir -p build/spike-hlog && (cd build/spike-hlog && ../../esp-isa-sim/configure --prefix=$PWD/../../install-hlog --enable-hcommitlog && make -j8 && make install)
```

然后在本仓库里编译基准并在 Spike 上验证、生成踪迹：

```bash
cd $PERF/rtl
make bench-n4096.riscv micro-n4096.riscv && make spike | grep -a VERIF     # 应打印 ALL VERIFIED
make -C rodinia all && for p in nn kmeans pgain pathfinder; do make -s -C rodinia $p.spike | grep -a PASS; done
cd .. && for p in nn kmeans pgain pathfinder; do python3 scripts/hwacha_trace.py run rtl/rodinia/$p.riscv -o rtl/results/trace-rodinia-$p.log; done
python3 scripts/compare_rodinia.py                                   # 现在可以直接用符号表
```

`kernels/rodinia/*.S` 由 `scripts/extract_hcc_kernel.py` 从 hwacha-cc 的 `.s` 抠出（寄存器配置取自 spike-hlog 的 `H: VSETCFG` 行），文件头有生成命令。

### 第三层：RTL 计时（Verilator 构建约 1 小时，每次计时 10 分钟到 2 小时）

```bash
cd ~/hwacha-compiler/chipyard
source ~/miniforge3/etc/profile.d/conda.sh && conda activate $PWD/.conda-env && source env.sh && export RISCV=$PWD/.conda-env/esp-tools
git -C generators/hwacha apply $PERF/rtl/patches/chipyard-hwacha-generator.patch          # 含 hwacha-compiler 的集成修复 + 本项目的 TileLink 跟踪与 IBoxML 修复
git -C generators/rocket-chip-inclusive-cache apply $PERF/rtl/patches/chipyard-inclusivecache-tl-trace.patch
git -C generators/rocket-chip apply $PERF/rtl/patches/chipyard-rocketchip-rocc-fpu.patch
cp $PERF/rtl/patches/HwachaLaneConfigs.scala generators/chipyard/src/main/scala/config/
cd sims/verilator
for c in HwachaRocketConfig HwachaL2RocketConfig HwachaL4RocketConfig HwachaL8RocketConfig; do make CONFIG=$c SIM_OPT_CXXFLAGS=-O1 -j4; done
make CONFIG=HwachaL16RocketConfig SIM_OPT_CXXFLAGS=-O1 VERILATOR_THREADS=4 -j12   # 16 lane 太大，单线程仿真每周期约 5 倍慢；4 线程约快 2–3 倍（构建 1 小时）
make CONFIG=HwachaNoVRURocketConfig SIM_OPT_CXXFLAGS=-O1 -j4   # 可选：无 VRU 与 2 bank L2（docs/24 §10.13）
make CONFIG=HwachaL2B2RocketConfig SIM_OPT_CXXFLAGS=-O1 -j4
```

（内存小于 16 GB 时用 `-j2` 并 `setsid nohup` 分离；第一次全量构建约 1 小时，之后每个配置增量约 5–15 分钟。）仿真器在 `sims/verilator/simulator-chipyard.harness-<Config>`，约 20k 周期/秒；同时跑的仿真不要超过 3–4 个，Verilator 多线程在超载时会因自旋等待慢 10 倍以上。

重新计时并与模型比较（`LANES=1/2/4` 选仿真器，日志写到 `rtl/results/`，文件名带 `-l2`/`-l4` 后缀）：

```bash
cd $PERF/rtl
make calibrate LANES=1          # Spike 验证 → 基准 + 微基准 RTL 计时 → compare_rtl.py（超过 MAXERR=12% 返回非零）
make calibrate LANES=2
make calibrate LANES=4          # 同样支持 LANES=8 / 16（16 lane 仿真器每周期约 5 倍慢于 1 lane）
make calibrate LANES=4 BANKS=2  # 多 bank L2：BANKS=2/4 选 HwachaL4B2/L4B4RocketConfig，日志后缀 -l4-b2；模型配置 configs/rtl-hwacha-rocket-l4b2.json
make -C rodinia rtl             # 四个 Rodinia 程序（三次计时；~1 小时；同样支持 LANES=）
make -C hcc rtl LANES=4         # hwacha-cc bench 内核
make probe6-rtl                 # store 布局探针（docs/24 10.8 节）
make tl-trace LANES=4           # TileLink 通道级跟踪：+verbose 下慢 30–50 倍，微基准约 30 分钟
```

每个内核计时三次（冷 / warm / warm2），比较脚本取 warm2（数据驻留 L2、没有 L1D 脏行探测干扰的稳态），模型对应稳态。同一台机器上重复计时的差别通常在 1% 以内，但 store 吞吐会随二进制布局在每 beat 1.03–1.16 拍之间变化（10.8 节），所以换了工具链版本或改了 `main.c` 后 store 密集内核的 RTL 周期数会有几个百分点的漂移，属正常。
