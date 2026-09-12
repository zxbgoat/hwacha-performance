"""Hwacha 性能模型。

模块概览：
- config     : 微架构与内存系统参数（与 docs/18-source-map.md 中的参数对应）
- isa        : Hwacha 工作线程指令的分类与汇编解析
- program    : 内核文件（向量取指块 + `# @` 指令描述的控制线程）
- hvl        : vsetcfg → 最大硬件向量长度
- memory     : L2 / DRAM / 预取的时序模型
- sim        : strip 粒度的周期级模拟器
- analytic   : 解析式上下界（roofline 风格），用于交叉校验
- cli        : 命令行入口
"""
from .config import HwachaConfig, MemoryConfig
from .program import load_kernel, Kernel
from .sim import Simulator, SimStats
from .analytic import analyze

__all__ = [
    "HwachaConfig", "MemoryConfig", "load_kernel", "Kernel",
    "Simulator", "SimStats", "analyze",
]
