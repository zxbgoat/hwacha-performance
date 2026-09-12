"""vsetcfg → 最大硬件向量长度（对应 rocc-unit.scala 的 epb 查找表逻辑）。"""
from __future__ import annotations
from dataclasses import dataclass
from .config import HwachaConfig


@dataclass(frozen=True)
class VCfg:
    v64: int = 1
    v32: int = 0
    v16: int = 0
    vp: int = 0

    @property
    def nvv(self) -> int:
        return self.v64 + self.v32 + self.v16

    def region(self, vreg_index: int) -> str:
        """架构向量寄存器号所在的精度区域：'d' / 's' / 'h'。"""
        if vreg_index < self.v64:
            return 'd'
        if vreg_index < self.v64 + self.v32:
            return 's'
        return 'h'


def max_vlen(cfg: HwachaConfig, vc: VCfg) -> int:
    """返回整机（所有 lane）最大硬件向量长度。ISA 保证至少 8。"""
    if vc.nvv == 0:
        return 0
    rows = cfg.n_sram_entries
    if cfg.conf_prec:
        # 一行 128 位放 2 个 d、4 个 w、8 个 h；按四分之一行加权
        weighted = 4 * vc.v64 + 2 * vc.v32 + vc.v16
        epb = (rows * 4) // weighted
    else:
        epb = rows // vc.nvv
    if vc.vp > 0:
        epb = min(epb, cfg.n_pred_entries // vc.vp)
    epb = max(epb, 1)
    per_lane = epb * cfg.n_strip
    per_lane = min(per_lane, cfg.max_vlen_per_lane)
    return max(per_lane, cfg.n_strip) * cfg.n_lanes


def rate_of(cfg: HwachaConfig, prec: str | None) -> int:
    """混合精度下窄精度操作每 strip 的元素倍率（1/2/4）。"""
    if not cfg.conf_prec or prec is None:
        return 1
    return {'d': 1, 's': 2, 'h': 4}.get(prec, 1)
