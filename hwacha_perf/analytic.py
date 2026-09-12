"""解析式上下界模型（roofline 风格），用于快速估计与交叉校验模拟结果。

对每个 stripmine 迭代（vl = maxvl）统计向量取指块的静态资源需求，
给出各资源的周期下界，取最大者为整体下界；再乘以迭代数。
"""
from __future__ import annotations
from dataclasses import dataclass, field
import math
from .config import HwachaConfig, MemoryConfig
from .isa import Instr, stages_for
from .program import Kernel
from .hvl import max_vlen, rate_of


@dataclass
class Bounds:
    maxvl: int
    iterations: int
    vl_last: int
    per_iter: dict = field(default_factory=dict)     # 资源 -> 每迭代周期
    total: dict = field(default_factory=dict)        # 资源 -> 总周期
    binding: str = ''
    lower_bound_cycles: int = 0
    fma_elems_per_iter: int = 0
    bytes_per_iter: int = 0

    def report(self) -> str:
        L = [f"maxvl={self.maxvl}, iterations={self.iterations}"]
        L.append("per-iteration lower bounds (cycles):")
        for k, v in sorted(self.per_iter.items(), key=lambda kv: -kv[1]):
            mark = '  <-- binding' if k == self.binding else ''
            L.append(f"    {k:22s} {v:10.1f}{mark}")
        L.append(f"total lower bound     : {self.lower_bound_cycles} cycles "
                 f"({self.lower_bound_cycles / max(1, self.iterations * self.maxvl):.3f} cycles/elem)")
        return "\n".join(L)


def analyze(kernel: Kernel, cfg: HwachaConfig | None = None, mcfg: MemoryConfig | None = None,
            n: int | None = None) -> Bounds:
    cfg = cfg or HwachaConfig()
    mcfg = mcfg or MemoryConfig()
    n = n if n is not None else kernel.n
    maxvl = max_vlen(cfg, kernel.vcfg)
    iters = kernel.iters or math.ceil(n / maxvl)
    vl = maxvl
    vl_lane = math.ceil(vl / cfg.n_lanes)
    U = cfg.n_strip
    vc = kernel.vcfg

    read_port = 0.0
    write_port = 0.0
    pred_port = 0.0
    fma = 0.0
    imul = fconv = fcmp = 0.0
    fdiv = idiv = 0.0
    fma_elems = 0
    lane_bytes = 0.0
    total_bytes = 0
    n_instr = 0
    seq_slot_cycles = 0.0
    lb = mcfg.line_bytes
    touched: dict[str, set] = {}     # 数组 -> 本迭代触及的行（相对数组基址）
    advancing: set[str] = set()      # 指针每次迭代前进的数组
    random_lines = 0                 # 索引访存：每迭代的行数
    random_cap = 0                   # 索引访存目标数组的总行数上限

    for ins in kernel.instrs:
        n_instr += 1
        if not ins.is_vector:
            continue
        rate = 1
        if cfg.conf_prec:
            regs = [r for r in list(ins.reads) + list(ins.writes) if r.startswith('vv')]
            rate = min((rate_of(cfg, vc.region(int(r[2:]))) for r in regs), default=rate_of(cfg, ins.prec))
        E = U * rate
        strips = math.ceil(vl_lane / E)
        k = ins.kind
        if ins.pred:
            pred_port += strips
        if k in ('fma', 'imul', 'fconv', 'fcmp', 'alu', 'cmp', 'plu', 'fdiv', 'idiv', 'rfirst', 'branch'):
            read_port += strips * ins.rp
            if ins.writes_vrf:
                write_port += strips
            if k == 'fma':
                fma += strips * cfg.n_banks / cfg.n_fma_units
                fma_elems += vl * (ins.seglen + 1)
            elif k == 'imul':
                imul += strips * cfg.n_banks
            elif k == 'fconv':
                fconv += strips * cfg.n_banks
            elif k == 'fcmp':
                fcmp += strips * cfg.n_banks
            elif k == 'fdiv':
                fdiv += vl_lane * cfg.fdiv_cycles_per_elem
            elif k == 'idiv':
                idiv += vl_lane * cfg.idiv_cycles_per_elem
        elif ins.is_mem:
            nb = ins.elsize * (ins.seglen + 1)
            if ins.mode == 'unit':
                beats = math.ceil(vl_lane * nb / cfg.tl_data_bytes)   # 假定基址对齐
            else:
                beats = vl_lane * (ins.seglen + 1)
            if ins.kind == 'amo':
                beats *= 2
            lane_bytes += beats * cfg.tl_data_bytes
            total_bytes += vl * nb * (2 if ins.kind == 'amo' else 1)
            # DRAM 足迹
            if ins.mode in ('unit', 'stride'):
                srcs = ins.srcs if ins.is_load else ins.srcs[1:]
                base_reg = srcs[0] if srcs else None
                d = kernel.va.get(base_reg) if base_reg else None
                if d is not None and d.kind == 'ptr':
                    arr = kernel.arrays[d.array]
                    off = d.offset_elems * arr.elem
                    if ins.mode == 'unit':
                        step = nb
                    else:
                        sreg = srcs[1] if len(srcs) > 1 else None
                        step = kernel.va_stride_bytes(sreg) if sreg else nb
                    lines = touched.setdefault(d.array, set())
                    if d.advance:
                        advancing.add(d.array)
                    if step <= lb:
                        lines.update(range(off // lb, (off + vl * step + lb - 1) // lb))
                    else:
                        lines.update((off + e * step) // lb for e in range(vl))
                else:
                    random_lines += (vl * nb + lb - 1) // lb
            else:
                g = ins.annot.get('gather', '').split()
                arr = kernel.arrays.get(g[0]) if g else None
                random_lines += min(vl, arr.nbytes // lb) if arr else vl
                random_cap += (arr.nbytes // lb) if arr else vl * iters
            if ins.is_store or ins.mode == 'indexed':
                read_port += strips * max(1, ins.rp)
            if ins.is_load:
                write_port += strips
        seq_slot_cycles += ins.slots

    # 跨迭代的 DRAM 足迹（下界：假定 L2 完美复用）：指针前进的数组以数组总行数为上限
    # （越界回绕表示复用），固定指针的数组只取一次。
    total_lines = 0
    for name, lines in touched.items():
        arr_lines = max(1, kernel.arrays[name].nbytes // lb)
        if name in advancing:
            total_lines += min(arr_lines, len(lines) * iters)
        else:
            total_lines += len(lines)
    total_lines += min(random_cap, random_lines * iters) if random_lines else 0
    dram_bytes = total_lines * lb / max(1, iters)      # 折算为每迭代
    per = {
        'vrf_read_port': read_port,
        'vrf_write_port': write_port,
        'pred_read_port': pred_port,
        'fma_units': fma,
        'imul_unit': imul,
        'fconv_unit': fconv,
        'fcmp_unit': fcmp,
        'fdiv_unit': fdiv,
        'idiv_unit': idiv,
        'lane_mem_port(128b)': lane_bytes / cfg.tl_data_bytes,
        'l2_bandwidth': total_bytes / (mcfg.l2_banks * cfg.tl_data_bytes),
        'dram_bandwidth': dram_bytes / (mcfg.dram_channels * mcfg.dram_bytes_per_cycle_per_channel),
        'scalar_issue': n_instr + len(kernel.va) + kernel.n_vmcs + 2,
        'control_thread': (kernel.ctrl_cycles if kernel.ctrl_cycles is not None else cfg.ctrl_cycles_per_iter)
                           + len(kernel.va) + kernel.n_vmcs + 2,
    }
    per = {k: v for k, v in per.items() if v > 0}
    binding = max(per, key=per.get)
    b = Bounds(maxvl=maxvl, iterations=iters, vl_last=n - (iters - 1) * maxvl, per_iter=per,
               total={k: v * iters for k, v in per.items()}, binding=binding,
               lower_bound_cycles=int(math.ceil(per[binding] * iters)),
               fma_elems_per_iter=fma_elems, bytes_per_iter=dram_bytes)
    return b
