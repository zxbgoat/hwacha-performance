#!/usr/bin/env python3
"""对 kernels/ 下所有内核在若干配置下运行模型，输出汇总表（Markdown）。

用法: python3 scripts/summary.py [--n N] [--kernels a.S,b.S] [--configs paper-28nm,open-source]
"""
import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from hwacha_perf import load_kernel, Simulator, analyze
from hwacha_perf.config import load_configs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=16384)
    ap.add_argument('--kernels', default=None)
    ap.add_argument('--configs', default='paper-28nm,open-source')
    ap.add_argument('--lanes', default='1')
    args = ap.parse_args()
    root = os.path.join(os.path.dirname(__file__), '..')
    if args.kernels:
        paths = [os.path.join(root, 'kernels', k) for k in args.kernels.split(',')]
    else:
        paths = sorted(glob.glob(os.path.join(root, 'kernels', '*.S')))
    print('| kernel | config | lanes | cycles | cyc/elem | GFLOPS | FMA util | B/cycle | bound cyc | binding |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for p in paths:
        k = load_kernel(p)
        for cname in args.configs.split(','):
            for L in [int(x) for x in args.lanes.split(',')]:
                cfg, mcfg = load_configs(os.path.join(root, 'configs', cname + '.json'), {'n_lanes': L})
                st = Simulator(k, cfg, mcfg, n=args.n).run()
                b = analyze(k, cfg, mcfg, n=args.n)
                print(f'| {k.name} | {cname} | {L} | {st.cycles} | {st.cycles_per_elem:.3f} | {st.gflops:.2f} | '
                      f'{100 * st.fma_util:.1f}% | {st.mem_bw_bytes_per_cycle:.2f} | {b.lower_bound_cycles} | {b.binding} |')


if __name__ == '__main__':
    main()
