"""Hwacha 工作线程指令的分类与汇编解析。

分类依据 docs/03-isa-reference.md 与 docs/modules/09-sequencer.md 的
"指令类别 → 序列器操作 / 功能单元" 映射。
"""
from __future__ import annotations
import re
from dataclasses import dataclass, field
from typing import Optional

REG_RE = re.compile(r'^(vv|vs|va|vp)(\d+)$')
FMA_RE = re.compile(r'^vf(add|sub|mul|madd|msub|nmsub|nmadd)\.([dsh])(?:\.([dsh]))?$')
FDIV_RE = re.compile(r'^vf(div|sqrt)\.([dsh])$')
FCVT_RE = re.compile(r'^vfcvt\.([dshwl]u?)\.([dshwl]u?)$')
FCMP_RE = re.compile(r'^vf(sgnj|sgnjn|sgnjx|min|max|class)\.([dsh])$')
VCMPF_RE = re.compile(r'^vcmpf(eq|lt|le)\.([dsh])$')
VCMPI_RE = re.compile(r'^vcmp(eq|lt|ltu|ez|nez)$')
VMEM_RE = re.compile(r'^v(l|s)(seg)?(st|x)?(b|h|w|d)(u)?$')
SMEM_RE = re.compile(r'^v(l|s)(a|s)(b|h|w|d)(u)?$')
AMO_RE = re.compile(r'^vamo(swap|add|and|or|xor|min|max|minu|maxu)\.([wd])$')
PLU_RE = re.compile(r'^vp(op|clear|set|(?:xor|or|and)(?:xor|or|and))$')
BR_RE = re.compile(r'^vcjal(r)?(?:\.(all|any))?$')

ALU_SET = {'vadd', 'vaddu', 'vsub', 'vsll', 'vsrl', 'vsra', 'vand', 'vor', 'vxor',
           'vslt', 'vsltu', 'veidx', 'vaddw', 'vsubw', 'vsllw', 'vsrlw', 'vsraw'}
IMUL_SET = {'vmul', 'vmulh', 'vmulhu', 'vmulhsu', 'vmulw'}
IDIV_SET = {'vdiv', 'vdivu', 'vrem', 'vremu', 'vdivw', 'vdivuw', 'vremw', 'vremuw'}
SCALAR_IMM = {'vaddi', 'vslli', 'vsrli', 'vsrai', 'vandi', 'vori', 'vxori', 'vslti',
              'vsltiu', 'vaddiw', 'vslliw', 'vsrliw', 'vsraiw', 'vlui', 'vauipc'}
WIDTH_BYTES = {'b': 1, 'h': 2, 'w': 4, 'd': 8}

# 序列器槽位占用（docs/modules/09-sequencer.md 表）
SLOTS = {
    'load': 3, 'store': 3, 'amo': 4, 'alu': 1, 'imul': 1, 'idiv': 2, 'fma': 1,
    'fdiv': 2, 'fconv': 1, 'fcmp': 1, 'cmp': 1, 'plu': 1, 'pmem': 3, 'rfirst': 1,
    'branch': 1,
}


class ParseError(Exception):
    pass


@dataclass
class Instr:
    mnemonic: str
    operands: list[str]
    kind: str                      # 见 SLOTS 的键，另有 scalar/sload/sstore/sfp/stop/fence
    pred: Optional[str] = None     # 'vp3'
    pred_neg: bool = False
    prec: Optional[str] = None     # 'd' | 's' | 'h'
    elsize: int = 8                # 访存元素字节数
    mode: str = ''                 # unit | stride | indexed（访存）
    seglen: int = 0
    dst: Optional[str] = None
    srcs: list[str] = field(default_factory=list)
    is_vector: bool = False
    rp: int = 0                    # 需要 SRAM 读口的向量源操作数个数
    slots: int = 0
    annot: dict = field(default_factory=dict)
    label: Optional[str] = None    # 分支目标
    line_no: int = 0
    text: str = ''

    # ---- 便捷属性 ----
    @property
    def is_mem(self) -> bool:
        return self.kind in ('load', 'store', 'amo', 'pmem')

    @property
    def is_load(self) -> bool:
        return self.kind in ('load', 'amo') or (self.kind == 'pmem' and self.mnemonic == 'vpl')

    @property
    def is_store(self) -> bool:
        return self.kind in ('store', 'amo') or (self.kind == 'pmem' and self.mnemonic == 'vps')

    @property
    def writes_vrf(self) -> bool:
        return self.dst is not None and self.dst.startswith('vv')

    @property
    def writes_prf(self) -> bool:
        return self.dst is not None and self.dst.startswith('vp')

    @property
    def reads(self) -> set[str]:
        s = {r for r in self.srcs if REG_RE.match(r)}
        if self.pred:
            s.add(self.pred)
        return s

    @property
    def writes(self) -> set[str]:
        return {self.dst} if self.dst and REG_RE.match(self.dst) else set()

    def dst_regs(self) -> list[str]:
        """分段访存写入 seglen+1 个连续寄存器。"""
        if not self.dst or not self.dst.startswith('vv'):
            return [self.dst] if self.dst else []
        n = int(self.dst[2:])
        return [f'vv{n + i}' for i in range(self.seglen + 1)]


def _is_vreg(x: str) -> bool:
    return x.startswith('vv')


def _clean_operand(tok: str) -> str:
    tok = tok.strip().rstrip(',')
    if tok.startswith('(') and tok.endswith(')'):
        tok = tok[1:-1]
    return tok


def parse_instr(text: str, line_no: int = 0) -> Optional[Instr]:
    """解析一行工作线程汇编。返回 None 表示空行/仅注释。"""
    annot: dict = {}
    if '#' in text:
        code, comment = text.split('#', 1)
        for m in re.finditer(r'@(\w+)(?:\s+([^@]+))?', comment):
            annot[m.group(1)] = (m.group(2) or '').strip()
    else:
        code = text
    code = code.strip()
    if not code or code.endswith(':'):
        return None
    toks = code.replace(',', ' ').split()
    pred, neg = None, False
    while toks and (toks[0].startswith('@') or toks[0].startswith('!') or REG_RE.match(toks[0]) and toks[0].startswith('vp') and len(toks) > 1 and not REG_RE.match(toks[1]) and toks[1] not in SCALAR_IMM):
        p = toks.pop(0)
        if p in ('@all', '@s'):
            continue
        neg = p.startswith('@!') or p.startswith('!')
        pred = p.lstrip('@!')
        if not REG_RE.match(pred) or not pred.startswith('vp'):
            raise ParseError(f'line {line_no}: bad predicate {p!r}')
    if not toks:
        return None
    mn = toks[0].lower()
    ops = [_clean_operand(t) for t in toks[1:]]
    ins = Instr(mnemonic=mn, operands=ops, kind='', pred=pred, pred_neg=neg,
                annot=annot, line_no=line_no, text=code)
    _classify(ins)
    return ins


def _classify(ins: Instr) -> None:
    mn, ops = ins.mnemonic, ins.operands

    if mn == 'vstop':
        ins.kind = 'stop'; return
    if mn == 'vfence':
        ins.kind = 'fence'; return
    m = BR_RE.match(mn)
    if m:
        ins.kind = 'branch'
        first = 1 if (not m.group(2) and len(ops) >= 3) else 0   # hwacha-cc 写法：vcjal <cond>, sd, label
        ins.dst = ops[first] if ops else None
        ins.label = ops[-1]
        ins.is_vector = True
        ins.slots = SLOTS['branch']
        return
    if mn in ('vpl', 'vps'):
        ins.kind = 'pmem'; ins.is_vector = True; ins.elsize = 1; ins.mode = 'unit'
        ins.dst = ops[0] if mn == 'vpl' else None
        ins.srcs = [ops[1]] + ([ops[0]] if mn == 'vps' else [])
        ins.slots = SLOTS['pmem']; ins.rp = 0
        return
    m = SMEM_RE.match(mn)
    if m:
        ld = m.group(1) == 'l'
        ins.kind = 'sload' if ld else 'sstore'
        ins.elsize = WIDTH_BYTES[m.group(3)]
        if ld:
            ins.dst, ins.srcs = ops[0], ops[1:]
        else:
            ins.srcs = ops
        return
    m = VMEM_RE.match(mn)
    if m:
        ld = m.group(1) == 'l'
        ins.kind = 'load' if ld else 'store'
        ins.elsize = WIDTH_BYTES[m.group(4)]
        ins.mode = {'st': 'stride', 'x': 'indexed', None: 'unit'}[m.group(3)]
        ins.is_vector = True
        if m.group(2):
            ins.seglen = int(ops[-1])
            ops = ops[:-1]
        if ld:
            ins.dst, ins.srcs = ops[0], ops[1:]
        else:
            ins.srcs = list(ops)      # 首操作数是 store 数据
        ins.rp = sum(1 for s in ins.srcs if _is_vreg(s))   # 索引向量 / store 数据
        ins.slots = SLOTS[ins.kind]
        ins.prec = {8: 'd', 4: 's', 2: 'h', 1: 'h'}[ins.elsize]
        return
    m = AMO_RE.match(mn)
    if m:
        ins.kind = 'amo'; ins.is_vector = True; ins.mode = 'indexed'
        ins.elsize = WIDTH_BYTES[m.group(2)]
        ins.dst, ins.srcs = ops[0], ops[1:]
        ins.rp = sum(1 for s in ins.srcs if _is_vreg(s))
        ins.slots = SLOTS['amo']
        ins.prec = 'd' if ins.elsize == 8 else 's'
        return
    m = FMA_RE.match(mn)
    if m:
        ins.kind = 'fma'; ins.prec = m.group(2)
    elif FDIV_RE.match(mn):
        ins.kind = 'fdiv'; ins.prec = FDIV_RE.match(mn).group(2)
    elif FCVT_RE.match(mn):
        ins.kind = 'fconv'
        a = FCVT_RE.match(mn).group(1)
        ins.prec = a[0] if a[0] in 'dsh' else 'd'
    elif FCMP_RE.match(mn):
        ins.kind = 'fcmp'; ins.prec = FCMP_RE.match(mn).group(2)
    elif VCMPF_RE.match(mn):
        ins.kind = 'fcmp'; ins.prec = VCMPF_RE.match(mn).group(2)
    elif VCMPI_RE.match(mn):
        ins.kind = 'cmp'; ins.prec = 'd'
    elif mn in ALU_SET:
        ins.kind = 'alu'; ins.prec = 's' if mn.endswith('w') else 'd'
    elif mn in IMUL_SET:
        ins.kind = 'imul'; ins.prec = 's' if mn.endswith('w') else 'd'
    elif mn in IDIV_SET:
        ins.kind = 'idiv'; ins.prec = 's' if mn.endswith('w') else 'd'
    elif PLU_RE.match(mn):
        ins.kind = 'plu'; ins.is_vector = True
    elif mn == 'vfirst':
        ins.kind = 'rfirst'; ins.is_vector = True
    elif mn in SCALAR_IMM:
        ins.kind = 'scalar'
    else:
        raise ParseError(f'line {ins.line_no}: unknown mnemonic {mn!r}')

    if ops:
        ins.dst, ins.srcs = ops[0], ops[1:]
    if ins.kind in ('cmp', 'fcmp') and ins.dst and ins.dst.startswith('vp'):
        pass
    if ins.kind == 'plu':
        ins.srcs = [s for s in ins.srcs if REG_RE.match(s)]
    if mn in ('vcmpez', 'vcmpnez'):
        ins.srcs = ins.srcs + ['vs0']
    if ins.kind == 'scalar':
        return
    vec = any(_is_vreg(o) for o in ops) or ins.kind in ('plu', 'rfirst')
    if ins.kind in ('cmp', 'fcmp', 'plu', 'rfirst') and not any(_is_vreg(o) for o in ops) and ins.kind not in ('plu', 'rfirst'):
        vec = False
    ins.is_vector = vec
    if not vec:
        # 全部为共享寄存器：标量指令，在标量单元执行
        ins.kind = 'sfp' if ins.kind in ('fma', 'fdiv', 'fconv', 'fcmp') else (
            'smuldiv' if ins.kind in ('imul', 'idiv') else 'scalar')
        return
    ins.rp = sum(1 for s in ins.srcs if _is_vreg(s))
    ins.slots = SLOTS[ins.kind]


def stages_for(ins: Instr, cfg) -> int:
    """定延迟功能单元的流水级数。"""
    k = ins.kind
    if k in ('alu', 'cmp'):
        return cfg.stages_alu
    if k == 'plu':
        return cfg.stages_plu
    if k == 'imul':
        return cfg.stages_imul
    if k == 'fma':
        return {'d': cfg.stages_dfma, 's': cfg.stages_sfma, 'h': cfg.stages_hfma}[ins.prec or 'd']
    if k == 'fconv':
        return cfg.stages_fconv
    if k == 'fcmp':
        return cfg.stages_fcmp
    return 1


# 共享功能单元与操作数锁存器分配（docs/modules/10-expander.md 第 6 节）
LATCH_GROUPS = {
    'fma0': (0, 1, 2), 'imul': (0, 1), 'fconv': (2,),
    'fma1': (3, 4, 5), 'vqu': (3, 4), 'fcmp': (3, 4), 'vgu': (5,),
}
