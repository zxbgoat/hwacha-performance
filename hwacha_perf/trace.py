"""Spike（hlog 构建 + 本仓库补丁）指令级踪迹的解析，与 cc/src/trace.cc 同一格式。"""
from __future__ import annotations
import re
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class TraceInstr:
    pc: int
    inst: int
    next: int
    vl: int
    scalar: bool
    active: list = field(default_factory=list)          # 每元素 0/1
    mem: list = field(default_factory=list)             # [(ut, addr)]

    def taken(self) -> bool:
        return self.next != self.pc + 8

    def active_at(self, e: int) -> bool:
        return self.scalar or e >= len(self.active) or bool(self.active[e])


@dataclass
class TraceBlock:
    start_pc: int
    vl: int
    instrs: list = field(default_factory=list)
    base_pc: Optional[int] = None     # 内核文件第一条指令对应的地址（多入口块时 != start_pc）

    def index_of(self, pc: int) -> int:
        return (pc - (self.base_pc if self.base_pc is not None else self.start_pc)) // 8


_WT = re.compile(r'^H: WT pc=([0-9a-f]+) inst=([0-9a-f]+) next=([0-9a-f]+) vl=(\d+) act=(\S*)')


def load_trace(path: str, lo: int = 0, hi: int = 1 << 64, blocks: Optional[tuple] = None,
               base: Optional[int] = None) -> list[TraceBlock]:
    """base：内核文件第一条指令的地址；不给则每个块以自身起点为基址（单入口块）。"""
    out: list[TraceBlock] = []
    cur: Optional[TraceBlock] = None
    expect = None
    pending: list = []

    def flush():
        nonlocal cur
        if cur is not None and cur.instrs and lo <= cur.start_pc < hi:
            out.append(cur)
        cur = None

    with open(path, errors='replace') as f:
        for line in f:
            if line.startswith('H: WT '):
                m = _WT.match(line)
                if not m:
                    continue
                pc, inst, nxt, vl = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4))
                act = m.group(5)
                if cur is None or pc != expect:
                    flush()
                    cur = TraceBlock(pc, vl, base_pc=base)
                ti = TraceInstr(pc, inst, nxt, vl, act == '-')
                if act != '-':
                    ti.active = [(int(act[i // 4], 16) >> (i % 4)) & 1 if i // 4 < len(act) else 0 for i in range(vl)]
                ti.mem = pending
                pending = []
                cur.instrs.append(ti)
                expect = nxt
                if (inst & 0xfff) == 0xc3f and (inst >> 12) == 0:
                    flush()
            elif line.startswith('HMEM:'):
                if line.startswith('HMEM: read from '):
                    addr = int(line[16:].split()[0], 16)
                elif line.startswith('HMEM: write '):
                    addr = int(line[12:].split()[0], 16)
                elif line.startswith('HMEM: rmw '):
                    addr = int(line[10:].split()[0], 16)
                else:
                    continue
                mu = re.search(r'ut=(\d+)', line)
                pending.append((int(mu.group(1)) if mu else 0, addr))
    flush()
    if blocks:
        out = out[blocks[0]:blocks[1]]
    return out
