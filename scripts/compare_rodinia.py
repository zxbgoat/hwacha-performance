#!/usr/bin/env python3
"""踪迹驱动模式下比较 Rodinia/hwacha-cc 内核：RTL 三次计时（rtl/rodinia，warm2 为稳态）与 C++ 模型。

用法: python3 scripts/compare_rodinia.py [--config configs/rtl-hwacha-rocket.json] [--max-err X]
踪迹：rtl/results/trace-rodinia-<prog>.log（scripts/hwacha_trace.py run rtl/rodinia/<prog>.riscv）；
每个程序把内核跑三次，踪迹里取中间一次（第 2 次）的 vf 块与 RTL 的 *_warm2 比较。
"""
import argparse, json, os, re, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
NM = os.path.join(os.path.expanduser(os.environ.get('HWACHA_ROOT', '~/hwacha-compiler')), 'chipyard/.conda-env/esp-tools/bin/riscv64-unknown-elf-nm')

_WT = re.compile(r'^H: WT pc=([0-9a-f]+) inst=([0-9a-f]+) next=([0-9a-f]+)')

def count_blocks(path, lo, hi):
    """踪迹里起始 pc 落在 [lo, hi) 的 vf 块数（与 cc/src/trace.cc 的切块规则一致：pc 不连续即新块，vstop 结束块）"""
    n = 0; expect = None; start = None
    for line in open(path, errors='replace'):
        if not line.startswith('H: WT '):
            continue
        m = _WT.match(line)
        if not m:
            continue
        pc, inst, nxt = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
        if start is None or pc != expect:
            if start is not None and lo <= start < hi: n += 1
            start = pc
        expect = nxt
        if (inst & 0xfff) == 0xc3f and (inst >> 12) == 0:
            if lo <= start < hi: n += 1
            start = None
    if start is not None and lo <= start < hi: n += 1
    return n

# (内核文件名, 程序, 起始符号, 结束符号, RTL 标签)
KERNELS = [
    ('nn',          'nn',         'NearestNeighbor_wt',  'NearestNeighbor_ct', 'nn hwacha-cc'),
    ('kmeans_swap', 'kmeans',     'kmeans_swap_wt',      'kmeans_kernel_c_ct', 'kmeans_swap hwacha-cc'),
    ('kmeans_c',    'kmeans',     'kmeans_kernel_c_wt',  'kmeans_swap_wt',     'kmeans_c hwacha-cc'),
    ('pgain',       'pgain',      'pgain_kernel_wt',     'memset_kernel_ct',   'pgain hwacha-cc'),
    ('pathfinder',  'pathfinder', 'dynproc_kernel_wt',   'dynproc_kernel_ct',  'pathfinder hwacha-cc'),
]

def syms(elf):
    out = subprocess.run([NM, '-n', elf], capture_output=True, text=True).stdout
    return {n: int(a, 16) for a, t, n in (l.split() for l in out.splitlines() if len(l.split()) == 3) if t in 'tT'}

def rtl_results(path):
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, errors='replace'):
        m = re.search(r'([\w ]+hwacha-cc(?:_warm2?)?): (\d+) cycles', line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out

def run_model(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    try:
        return json.loads(r.stdout)['cycles']
    except Exception:
        sys.stderr.write(r.stderr[-400:] + '\n')
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default=os.path.join(ROOT, 'configs', 'rtl-hwacha-rocket.json'))
    ap.add_argument('--no-py', action='store_true', help='（已无作用，Python 模型已删除）')
    ap.add_argument('--rep', type=int, default=1, help='取第几次运行的踪迹块（0 冷 / 1 warm / 2 warm2）')
    ap.add_argument('--max-err', type=float, default=None)
    ap.add_argument('--syms', default=None, help='不用 nm 而直接给出各内核的踪迹地址范围：name=lo:hi,...（十六进制）')
    a = ap.parse_args()
    fixed = {}
    for kv in (a.syms.split(',') if a.syms else []):
        name, rng = kv.split('='); lo, hi = rng.split(':'); fixed[name] = (int(lo, 16), int(hi, 16))
    print(f"{'kernel':<13}{'blocks':>7}{'RTL cold':>10}{'RTL warm2':>10}{'C++':>9}{'err':>8}")
    errs = []
    for name, prog, s0, s1, tag in KERNELS:
        if name in fixed:
            lo, hi = fixed[name]
        else:
            elf = os.path.join(ROOT, 'rtl', 'rodinia', prog + '.riscv')
            sm = syms(elf); lo, hi = sm[s0], sm[s1]
        trace = os.path.join(ROOT, 'rtl', 'results', f'trace-rodinia-{prog}.log')
        nblk = count_blocks(trace, lo, hi)
        k = nblk // 3
        b0, b1 = a.rep * k, (a.rep + 1) * k
        kp = os.path.join(ROOT, 'kernels', 'rodinia', name + '.S')
        common = [kp, '--config', a.config, '--trace', trace, '--trace-range', f'{lo:x}:{hi:x}', '--trace-blocks', f'{b0}:{b1}', '--json']
        c = run_model([os.path.join(ROOT, 'cc', 'build', 'hwacha-sim'), 'run'] + common + ['--quiet'])
        res = rtl_results(os.path.join(ROOT, 'rtl', 'results', f'rodinia-{prog}.log'))
        cold, warm = res.get(tag), res.get(tag + '_warm2')
        def e(v): return f"{100*(v-warm)/warm:+.1f}%" if (v and warm) else 'n/a'
        print(f"{name:<13}{k:>7}{cold or '-':>10}{warm or '-':>10}{c or 'ERR':>9}{e(c):>8}")
        if c and warm: errs.append(abs(100*(c-warm)/warm))
    if errs:
        print(f"C++ mean |err| = {sum(errs)/len(errs):.1f}%, max = {max(errs):.1f}%")
        if a.max_err is not None and max(errs) > a.max_err:
            sys.exit(1)

if __name__ == '__main__':
    main()
