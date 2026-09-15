#!/usr/bin/env python3
"""对 kernels/ 下所有内核在若干配置下运行 C++ 模型，输出汇总表（Markdown）。

用法: python3 scripts/summary.py [--n N] [--kernels a.S,b.S] [--configs paper-28nm,open-source] [--lanes 1,2,4]
"""
import argparse, glob, json, os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
EXE = os.path.join(ROOT, 'cc', 'build', 'hwacha-sim')

def run(kernel, cfg, n, lanes):
    r = subprocess.run([EXE, 'run', kernel, '--config', cfg, '--n', str(n), '--lanes', str(lanes), '--quiet', '--json'], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        sys.stderr.write(f'{kernel}: {r.stderr[-300:]}\n'); return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=16384)
    ap.add_argument('--kernels', default=None)
    ap.add_argument('--configs', default='paper-28nm,open-source')
    ap.add_argument('--lanes', default='1')
    a = ap.parse_args()
    paths = [os.path.join(ROOT, 'kernels', k) for k in a.kernels.split(',')] if a.kernels else sorted(glob.glob(os.path.join(ROOT, 'kernels', '*.S')))
    print('| kernel | config | lanes | cycles | cyc/elem | GFLOPS | FMA util | B/cycle |')
    print('|---|---|---|---|---|---|---|---|')
    for p in paths:
        for cname in a.configs.split(','):
            for L in [int(x) for x in a.lanes.split(',')]:
                d = run(p, os.path.join(ROOT, 'configs', cname + '.json'), a.n, L)
                if not d: continue
                print(f"| {os.path.basename(p)[:-2]} | {cname} | {L} | {d['cycles']} | {d['cycles_per_elem']:.3f} | {d['gflops']:.2f} | "
                      f"{100 * d['fma_util']:.1f}% | {d['mem_bw_bytes_per_cycle']:.2f} |")

if __name__ == '__main__':
    main()
