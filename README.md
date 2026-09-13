# hwacha-performance

UC Berkeley Hwacha 解耦向量取指加速器的文档集与性能模型。

- `docs/`：设计思想、编程模型、ISA、整体架构、微架构与各子模块文档（从 `docs/README.md` 进入）
- `hwacha_perf/`：Python 版 strip 粒度性能模拟器 + 解析式上下界模型（说明见 `docs/22-performance-model.md`）
- `cc/`：C++ 版 gem5 风格事件驱动周期级模型（事件队列、Port/Packet、逐拍仲裁的 L2、JEDEC 时序的 DRAM 控制器；说明见 `docs/23-cpp-model.md`）
- `kernels/`：用 Hwacha 汇编写的示例内核（vvadd、saxpy、daxpy、csaxpy、dgemm 分块、模板滤波、gather、FMA 峰值）
- `configs/`：论文评估配置、开源主线配置、混合精度配置、理想内存配置
- `tests/`：pytest 回归测试
- `scripts/summary.py`：批量运行并输出汇总表

## 快速开始

```bash
pip install -e .                                   # 或直接 python3 -m hwacha_perf.cli
hwacha-perf run kernels/daxpy.S --bounds           # 模拟 + 解析下界
hwacha-perf run kernels/dgemm_opt.S --lanes 4 --n 131072
hwacha-perf sweep kernels/daxpy.S --lanes 1,2,4 --vru on,off
hwacha-perf run kernels/saxpy.S --config configs/paper-28nm-mxp.json
hwacha-perf run kernels/vvadd.S --set n_seq_entries=16 --set mem.dram_latency=80 --json
python3 scripts/summary.py --n 16384
python3 -m pytest -q

# C++ 模型
cd cc && mkdir -p build && cd build && cmake -G Ninja .. && ninja && ctest
./hwacha-sim run ../../kernels/daxpy.S --n 65536 --json
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
