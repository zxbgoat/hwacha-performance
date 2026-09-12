"""模型参数。默认值取自 ucb-bar/hwacha 的 DefaultHwachaConfig 与论文评估配置。"""
from __future__ import annotations
from dataclasses import dataclass, asdict, fields
import json
from typing import Any


@dataclass
class HwachaConfig:
    # ---- 机器组织 ----
    n_lanes: int = 1                 # HwachaNLanes
    n_banks: int = 4                 # HwachaNBanks
    bank_width: int = 128            # HwachaBankWidth（位）
    reg_len: int = 64                # HwachaRegLen
    n_sram_entries: int = 256        # HwachaNSRAMRFEntries（每 bank 行数）
    n_pred_entries: int = 256        # HwachaNPredRFEntries
    n_vector_regs: int = 256
    n_pred_regs: int = 16
    n_seq_entries: int = 8           # HwachaNSeqEntries（主序列器槽位）
    n_fma_units: int = 2             # VFU0 的 FMA0 与 VFU1 的 FMA1
    conf_prec: bool = False          # HwachaConfPrec（混合精度）
    # ---- 功能单元流水级数 ----
    stages_alu: int = 1
    stages_plu: int = 0
    stages_imul: int = 3
    stages_dfma: int = 4
    stages_sfma: int = 3
    stages_hfma: int = 3
    stages_fconv: int = 2
    stages_fcmp: int = 1
    # 变延迟单元：每 lane 各一个非流水单元，按元素串行
    fdiv_cycles_per_elem: int = 22   # hardfloat DivSqrtRecF64 近似
    idiv_cycles_per_elem: int = 65   # rocket MulDiv 64 位除法近似
    # ---- 前端与命令队列 ----
    cmdq_len: int = 32               # HwachaCMDQLen
    vf_fetch_latency: int = 2        # vf 命令到第一条工作线程指令译码
    scalar_smu_latency: int = 30     # 标量 load 经 SMU 访问 L2
    scalar_fpu_latency: int = 8      # 经 FPREQQ/FPRESPQ 使用 Rocket FPU
    scalar_muldiv_latency: int = 8
    branch_resolve_latency: int = 4  # 谓词归约汇总到标量单元
    ctrl_cycles_per_iter: int = 6    # 控制线程每次 stripmine 的地址簿记/循环开销（命令本身另计）
    # ---- VMU / DCC ----
    n_vmt_entries: int = 64          # HwachaNVMTEntries：每 lane 在途 beat 上限
    vmu_issue_latency: int = 4       # IBox → ABox0 → 翻译 → 首个请求
    vlu_latency: int = 3             # 数据返回 → 写回 VRF
    vsdq_strips: int = 2             # VSU 允许领先 VMU 发送的 strip 数（VSDQ 深度近似）
    tl_data_bytes: int = 16          # TileLink 数据宽度（128 位）
    # ---- VRU ----
    build_vru: bool = True           # 论文配置；开源主线实际关闭（见 docs/modules/08-vru.md）
    vru_max_outstanding: int = 20    # HwachaVRUMaxOutstandingPrefetches
    vru_early_ignore: int = 1        # HwachaVRUEarlyIgnore
    vru_max_runahead_bytes: int = 1 << 24
    # ---- 频率（仅用于换算 GFLOPS） ----
    freq_ghz: float = 1.0

    @property
    def n_slices(self) -> int:
        return self.bank_width // self.reg_len

    @property
    def n_strip(self) -> int:
        """一个 strip 的 64 位元素数（= nBanks × nSlices，默认 8）。"""
        return self.n_banks * self.n_slices

    @property
    def max_vlen_per_lane(self) -> int:
        return self.n_banks * self.n_sram_entries * self.bank_width // self.reg_len


@dataclass
class MemoryConfig:
    l2_banks: int = 4
    l2_bytes_per_bank: int = 256 * 1024
    l2_ways: int = 8
    line_bytes: int = 64
    l2_hit_latency: int = 24         # ccbench 校准值 ~22–24 周期
    l2_trackers_per_bank: int = 16   # 每 bank 在途 miss 上限
    dram_latency: int = 110          # ~110 ns @ 1 GHz
    dram_channels: int = 2
    dram_bytes_per_cycle_per_channel: float = 3.73  # LPDDR3-933 ×32 位 @ 1 GHz 核心时钟
    page_bytes: int = 4096
    tlb_entries: int = 8             # HwachaNDTLB
    tlb_miss_latency: int = 40       # PTW 近似
    l2_supports_amo: bool = True

    @property
    def l2_total_bytes(self) -> int:
        return self.l2_banks * self.l2_bytes_per_bank


def _apply(obj: Any, overrides: dict) -> Any:
    names = {f.name for f in fields(obj)}
    for k, v in overrides.items():
        if k not in names:
            raise KeyError(f"unknown parameter: {k}")
        setattr(obj, k, type(getattr(obj, k))(v) if not isinstance(getattr(obj, k), bool) else bool(v))
    return obj


def load_configs(path: str | None = None, hw_overrides: dict | None = None,
                 mem_overrides: dict | None = None) -> tuple[HwachaConfig, MemoryConfig]:
    """从 JSON 文件（{"hwacha": {...}, "memory": {...}}）与覆盖字典构造配置。"""
    hw, mem = HwachaConfig(), MemoryConfig()
    if path:
        with open(path) as f:
            data = json.load(f)
        _apply(hw, data.get("hwacha", {}))
        _apply(mem, data.get("memory", {}))
    if hw_overrides:
        _apply(hw, hw_overrides)
    if mem_overrides:
        _apply(mem, mem_overrides)
    return hw, mem


def dump_configs(hw: HwachaConfig, mem: MemoryConfig) -> dict:
    return {"hwacha": asdict(hw), "memory": asdict(mem)}
