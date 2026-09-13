"""内核文件：向量取指块汇编 + `# @` 指令描述的控制线程。

指令（写在以 `#` 开头的注释中）：
  @kernel <name>
  @n <elements>                     应用向量总长度
  @cfg v64=<n> v32=<n> v16=<n> vp=<n>
  @array <name> elem=<bytes> n=<elements> [base=<hex>]
  @va <vaK> = <array> [stride=<elems>] [offset=<elems>] [fixed]   指针（每次迭代前进 vl×elem×stride）
  @va <vaK> = <integer>                              常量（例如字节步长）
  @vs <n>                            每次迭代的 vmcs 条数（默认 0，循环外的不计）
  @ctrl <cycles>                     每次迭代控制线程额外开销（默认取配置）
  @iters <n>                         覆盖 stripmine 迭代次数
  @keep                              vf 块之间无重复加载（仅用于说明，不影响模型）
指令行注释：
  @taken <n>            一致性分支每次 vf 中被采纳的次数
  @gather <array> [random|unit]     索引访存的目标数组与访问模式
"""
from __future__ import annotations
import re
from dataclasses import dataclass, field
from typing import Optional
from .isa import Instr, parse_instr, ParseError
from .hvl import VCfg


@dataclass
class ArrayDef:
    name: str
    elem: int
    n: int
    base: int = 0

    @property
    def nbytes(self) -> int:
        return self.elem * self.n


@dataclass
class VaDef:
    reg: str
    kind: str                       # 'ptr' | 'const'
    array: Optional[str] = None
    value: int = 0
    stride_elems: int = 1
    advance: bool = True
    offset_elems: int = 0


@dataclass
class Kernel:
    name: str = 'kernel'
    n: int = 4096
    vcfg: VCfg = VCfg(1, 0, 0, 0)
    arrays: dict[str, ArrayDef] = field(default_factory=dict)
    va: dict[str, VaDef] = field(default_factory=dict)
    n_vmcs: int = 0
    ctrl_cycles: Optional[int] = None
    iters: Optional[int] = None
    instrs: list[Instr] = field(default_factory=list)
    labels: dict[str, int] = field(default_factory=dict)
    path: str = ''

    def array_of_va(self, reg: str) -> Optional[ArrayDef]:
        d = self.va.get(reg)
        if d and d.kind == 'ptr':
            return self.arrays[d.array]
        return None

    def va_stride_bytes(self, reg: str) -> int:
        d = self.va.get(reg)
        if d is None:
            return 8
        if d.kind == 'const':
            return d.value
        return self.arrays[d.array].elem * d.stride_elems


_KV = re.compile(r'(\w+)=([\w.]+)')


def _parse_int(s: str) -> int:
    return int(s, 0)


def parse_kernel(text: str, path: str = '') -> Kernel:
    k = Kernel(path=path)
    next_base = 0x8000_0000
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith('#'):
            body = line[1:].strip()
            if body.startswith('@'):
                parts = body[1:].split(None, 1)
                key = parts[0]
                arg = parts[1].strip() if len(parts) > 1 else ''
                if key == 'kernel':
                    k.name = arg
                elif key == 'n':
                    k.n = _parse_int(arg)
                elif key == 'cfg':
                    kv = {a: int(b) for a, b in _KV.findall(arg)}
                    k.vcfg = VCfg(kv.get('v64', 0), kv.get('v32', 0), kv.get('v16', 0), kv.get('vp', 0))
                elif key == 'array':
                    name, rest = arg.split(None, 1) if ' ' in arg else (arg, '')
                    kv = dict(_KV.findall(rest))
                    a = ArrayDef(name, int(kv.get('elem', 8)), _parse_int(kv.get('n', str(k.n))))
                    if 'base' in kv:
                        a.base = _parse_int(kv['base'])
                    else:
                        a.base = next_base
                        next_base = (next_base + a.nbytes + 0xFFFF) & ~0xFFF
                    k.arrays[name] = a
                elif key == 'va':
                    m = re.match(r'(va\d+)\s*=\s*(\S+)(.*)', arg)
                    if not m:
                        raise ParseError(f'line {ln}: bad @va')
                    reg, target, rest = m.group(1), m.group(2), m.group(3)
                    if target in k.arrays:
                        kv = dict(_KV.findall(rest))
                        k.va[reg] = VaDef(reg, 'ptr', array=target,
                                          stride_elems=int(kv.get('stride', 1)),
                                          advance='fixed' not in rest,
                                          offset_elems=int(kv.get('offset', 0)))
                    else:
                        k.va[reg] = VaDef(reg, 'const', value=_parse_int(target))
                elif key == 'vs':
                    k.n_vmcs = int(arg)
                elif key == 'ctrl':
                    k.ctrl_cycles = int(arg)
                elif key == 'iters':
                    k.iters = int(arg)
                elif key == 'keep':
                    pass
                else:
                    raise ParseError(f'line {ln}: unknown directive @{key}')
            continue
        if not line:
            continue
        m = re.match(r'^(\.?[A-Za-z_]\w*):\s*(.*)$', line)
        if m:
            k.labels[m.group(1)] = len(k.instrs)
            line = m.group(2)
            if not line:
                continue
        if line.startswith('.'):
            continue
        ins = parse_instr(line, ln)
        if ins is not None:
            k.instrs.append(ins)
    _validate(k)
    return k


def _validate(k: Kernel) -> None:
    if not k.instrs or k.instrs[-1].kind != 'stop':
        # 允许 vstop 后面跟别的块，但块尾必须有 vstop
        if not any(i.kind == 'stop' for i in k.instrs):
            raise ParseError('vector-fetch block must end with vstop')
    for ins in k.instrs:
        if ins.kind == 'branch' and ins.label not in k.labels:
            raise ParseError(f'line {ins.line_no}: unknown label {ins.label}')
        if ins.is_mem and ins.mode in ('unit', 'stride'):
            base = ins.srcs[-2] if (ins.mode == 'stride' and ins.kind == 'store') else ins.srcs[0] if ins.kind == 'load' else ins.srcs[1]
            if ins.kind == 'load' and ins.mode == 'stride':
                base = ins.srcs[0]
            if base.startswith('va') and base not in k.va:
                raise ParseError(f'line {ins.line_no}: {base} not described by @va')


def load_kernel(path: str) -> Kernel:
    with open(path) as f:
        return parse_kernel(f.read(), path)
