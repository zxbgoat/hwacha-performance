# 19 工具链、Chipyard 集成、测试与仿真

## 1. 软件栈总览

Hwacha 是非标准扩展，上游 GCC/LLVM/Spike 都不认识它的指令，必须用 Berkeley 的 **esp-tools** 分支：

| 仓库 | 作用 |
|---|---|
| `ucb-bar/esp-tools` | 元仓库，把下面几个作为子模块，提供 `build.sh` |
| `ucb-bar/riscv-gnu-toolchain`（esp 分支） | binutils 识别 Hwacha 汇编（`vsetcfg`、`@p`/`@all`/`@s` 前缀、`vf` 等） |
| `ucb-bar/esp-isa-sim` | Spike 分支，`--extension=hwacha`，`hwacha/` 目录是功能模拟器 |
| `ucb-bar/esp-opcodes` | `opcodes-hwacha` 指令编码源，生成 `encodings_hwacha.h` 与 `inst-hwacha.scala` |
| `ucb-bar/esp-tests` | riscv-tests 扩展：`isa/rv64uv`（向量用户态）、`rv64sv`（向量特权态）、`rv64uzfh`（半精度）；`benchmarks/vec-*` |
| `ucb-bar/esp-llvm` | 早期 LLVM 3.x 分支，仅生成汇编，非上游 |
| riscv-pk / fesvr | 代理内核与前端服务器 |

标量化 OpenCL 编译器（PoCL + LLVM 后端）与 torture 向量测试生成器未作为独立仓库公开，只在论文中描述；`riscv-torture` 子模块里有 Hwacha 支持。

## 2. 通过 Chipyard 使用

```bash
# 构建 Hwacha 工具链（安装到 chipyard 根目录的 esp-tools-install）
./scripts/build-toolchains.sh esp-tools
# 使用时切换环境
source env.sh   # 或 env-esp-tools.sh，视版本
```

SoC 配置中加入 config fragment：

```scala
class HwachaRocketConfig extends Config(
  new hwacha.DefaultHwachaConfig ++
  new freechips.rocketchip.subsystem.WithNBigCores(1) ++
  new chipyard.config.AbstractConfig)
```

要点：
- `DefaultHwachaConfig` 通过 `BuildRoCC` 把 `Hwacha` 挂到 tile 的 RoCC 口；默认经 System Bus 直连 L2。
- 多 tile 时用 `WithMultiRoCC` + `WithMultiRoCCHwacha(hartId)` 指定哪个 hart 带 Hwacha（Chipyard 文档 Heterogeneous SoCs 一节有 `LargeBoomAndHwachaRocketConfig` 例子）。
- 修改参数：写一个继承 `DefaultHwachaConfig` 的 Config 覆盖 `HwachaNLanes`、`HwachaConfPrec` 等 Field。
- AMO 需要真正的 L2（如 `WithInclusiveCache`），否则 broadcast hub 不支持。
- Chipyard 1.6.0 及之前版本文档中有 Hwacha 章节；更新版本保留了 generator，但 esp-tools 可能落后于上游工具。

## 3. hwacha-template（已归档）

独立于 Chipyard 的旧模板，README 里的流程仍是理解测试结构的好参考：

```bash
./scripts/init-submodules
export RISCV=/path/to/install; export PATH=$RISCV/bin:$PATH
cd riscv-tools && ./build.sh          # 构建 esp-tools
cd ../verisim && make                 # 生成 simulator-example-ExampleHwachaConfig
make run-asm-tests                    # 全部汇编测试
make run-bmark-tests                  # benchmark
make run-rv64uv-p-asm-tests           # 仅 Hwacha 用户态向量测试（物理内存）
make run-rv64uv-vp-asm-tests          # 虚拟内存下的向量测试
make run-asm-tests-debug              # 带波形
make rgentest R_SIM=../vsim/simv-...  # torture 随机测试
```

AMO 测试预期失败（见上）。

## 4. esp-tests 中的向量 benchmark

`benchmarks/` 下与 Hwacha 相关的：`vec-vvadd`、`vec-saxpy`、`vec-daxpy`、`vec-sdaxpy`、`vec-hsaxpy`、`vec-saxpy-streamx`、`vec-stream`、`vec-sgemm-naive`、`vec-sgemm-opt`、`vec-dgemm-opt`、`vec-dgemm-opt-multi`、`vec-sdgemm-opt`、`vec-hgemm-opt`、`vec-hsgemm-opt`、`mt-vvadd`、`mt-matmul`、`pb-sgemm`、`pb-spmv`。`ucb-bar/riscv-benchmarks/hwacha` 目录有另一份类似列表，Makefile 目标 `riscv: check-hwacha`。

每个 benchmark 通常是一个 C 文件（控制线程，用内联汇编发 `vsetcfg`/`vsetvl`/`vmca`/`vf`）加一个 `.S` 文件（向量取指块）。第三方仓库 `aignacio/hwacha_vvadd_benchmark` 是在 `vvadd` 上加了打印内部变量的教学版本。

## 5. Spike 功能模拟

```bash
spike -p1 --isa=rv64gc --extension=hwacha pk ./vec-vvadd.riscv
```

`esp-isa-sim/hwacha/` 目录实现了 Hwacha 的 ISA 级语义（`encodings_hwacha.h`、各指令的 `.h`），用于对拍 RTL。esp-tests 的 benchmark Makefile 默认就用这个命令。Gemmini 的 Spike 扩展也放在同一个 esp-isa-sim 中。

## 6. RTL 调试手段

- `HwachaCommitLog = true` 时仿真会打印 `H: VSETCFG[...]`、`H: VSETVL[...]` 等提交日志，以及每条工作线程指令的提交。
- 各主要模块（序列器、展开器）源码里有 `debug` 方法，打开后打印冒险位图与调度决策。
- `HwachaCounterIO` 预留了 RoCC、主序列器、VRU 的性能计数器接口（顶层部分连接被注释掉）。

## 7. 手写向量块的最小骨架

```c
// 控制线程（C 内联汇编）
asm volatile ("vsetcfg %0" :: "r"(cfg));          // 或用宏 VSETCFG(v64, pred)
for (...) {
  asm volatile ("vsetvl %0, %1" : "=r"(vl) : "r"(n));
  asm volatile ("vmca va0, %0" :: "r"(a));
  asm volatile ("vmca va1, %0" :: "r"(b));
  asm volatile ("vf 0(%0)" :: "r"(&vf_block));
  a += vl; b += vl; n -= vl;
}
asm volatile ("fence");
```

```asm
# 向量取指块（.S，8 字节对齐）
    .align 3
vf_block:
    vld  vv0, va0
    vld  vv1, va1
    vfadd.d vv0, vv0, vv1
    vsd  vv0, va1
    vstop
```

`esp-tests/isa/macros` 与 benchmark `common/` 目录里有现成的 `VSETCFG`、`VSETVL`、`VMCA` 等宏。

## 8. 其他集成点

- `ucb-bar/onnxruntime-riscv` 的 systolic runner 有可选的 Hwacha 后端，需要 esp-tools。
- `ucb-bar/hwacha-net` 是基于 Hwacha 的神经网络内核实验仓库。
- FireSim 可以对带 Hwacha 的 Chipyard 设计做 FPGA 加速仿真，工具链同样选 esp-tools。
