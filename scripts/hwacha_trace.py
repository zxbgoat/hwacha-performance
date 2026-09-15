#!/usr/bin/env python3
"""用带踪迹的 Spike（~/hwacha-compiler/install-hlog）运行 RISC-V 程序，导出指令级踪迹，并按 vf 块符号给出地址范围。

用法:
  python3 scripts/hwacha_trace.py run  <prog.riscv> -o trace.log          # 运行并保存踪迹
  python3 scripts/hwacha_trace.py range <prog.riscv> <symbol>              # 打印 lo:hi（下一个符号之前）
  python3 scripts/hwacha_trace.py stats <trace.log> [--range lo:hi]        # 每块指令数 / 活跃元素比例
"""
import argparse, os, re, subprocess, sys
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from logio import openlog, resolve
HW = os.path.expanduser(os.environ.get('HWACHA_ROOT', '~/hwacha-compiler'))
SPIKE = os.path.join(HW, 'install-hlog', 'bin', 'spike')
NM = os.path.join(HW, 'chipyard', '.conda-env', 'esp-tools', 'bin', 'riscv64-unknown-elf-nm')

def sym_range(elf, sym):
    out = subprocess.run([NM, '-n', elf], capture_output=True, text=True).stdout
    syms = [(int(a, 16), n) for a, t, n in (l.split() for l in out.splitlines() if len(l.split()) == 3) if t in 'tT']
    for i, (a, n) in enumerate(syms):
        if n == sym:
            hi = syms[i + 1][0] if i + 1 < len(syms) else a + 0x10000
            return a, hi
    raise SystemExit(f'symbol {sym} not found')

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('run'); p.add_argument('prog'); p.add_argument('-o', default='trace.log')
    p = sub.add_parser('range'); p.add_argument('prog'); p.add_argument('symbol')
    p = sub.add_parser('stats'); p.add_argument('trace'); p.add_argument('--range', default=None)
    a = ap.parse_args()
    if a.cmd == 'run':
        env = dict(os.environ, HWACHA_TRACE=os.path.abspath(a.o))
        r = subprocess.run([SPIKE, '--isa=rv64gc', '--extension=hwacha', a.prog], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        n = sum(1 for l in openlog(a.o) if l.startswith('H: WT '))
        print(f'{a.o}: {n} worker-thread instructions, exit {r.returncode}')
    elif a.cmd == 'range':
        lo, hi = sym_range(a.prog, a.symbol)
        print(f'{lo:x}:{hi:x}')
    else:
        lo, hi = (int(x, 16) for x in a.range.split(':')) if a.range else (0, 1 << 64)
        blocks, cur = [], None
        for l in openlog(a.trace):
            if not l.startswith('H: WT '):
                continue
            pc = int(re.search(r'pc=([0-9a-f]+)', l).group(1), 16)
            nxt = int(re.search(r'next=([0-9a-f]+)', l).group(1), 16)
            vl = int(re.search(r'vl=(\d+)', l).group(1))
            act = l.split('act=')[1].strip()
            na = None if act == '-' else sum(bin(int(c, 16)).count('1') for c in act)
            if cur is None or pc != cur['expect']:
                cur = {'pc': pc, 'vl': vl, 'n': 0, 'act': 0, 'vec': 0, 'expect': pc}
                blocks.append(cur)
            cur['n'] += 1; cur['expect'] = nxt
            if na is not None: cur['vec'] += 1; cur['act'] += na
            if re.search(r'inst=0+c3f ', l): cur = None
        sel = [b for b in blocks if lo <= b['pc'] < hi]
        print(f'{len(sel)} vf blocks in range')
        for b in sel[:20]:
            print(f"  pc={b['pc']:x} vl={b['vl']} instrs={b['n']} avg_active={b['act']/max(1,b['vec']*b['vl']):.2f}")

if __name__ == '__main__':
    main()
