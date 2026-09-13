#!/usr/bin/env python3
"""用 C++ 模型做几组设计空间研究，输出 Markdown 表（docs/25-design-space.md 的数据来源）。

用法: python3 scripts/design_space.py [--config configs/paper-28nm.json] [--n 65536] [--study seq,vmt,ports,vru,lanes]
"""
import argparse, json, os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
EXE = os.path.join(ROOT, 'cc', 'build', 'hwacha-sim')

def run(kernel, cfg, n, sets):
    cmd = [EXE, 'run', os.path.join(ROOT, 'kernels', kernel + '.S'), '--config', cfg, '--n', str(n), '--quiet', '--json']
    for k, v in sets.items():
        cmd += ['--set', f'{k}={str(v).lower() if isinstance(v, bool) else v}']
    r = subprocess.run(cmd, capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        sys.stderr.write(f'{kernel} {sets}: {r.stderr[-300:]}\n'); return None

def table(title, kernels, cfg, n, variants, metric='cycles'):
    print(f'\n### {title}\n')
    print('| kernel | ' + ' | '.join(v[0] for v in variants) + ' |')
    print('|---|' + '---|' * len(variants))
    for k in kernels:
        cells = []
        base = None
        for name, sets in variants:
            r = run(k, cfg, n, sets)
            v = r[metric] if r else None
            if base is None: base = v
            if v is None: cells.append('ERR')
            elif metric == 'cycles' and base: cells.append(f'{v} ({v/base:.2f}×)' if v != base else f'{v}')
            elif metric == 'gflops' and base: cells.append(f'{v:.2f} ({v/base:.2f}×)' if v != base else f'{v:.2f}')
            else: cells.append(f'{v:.2f}' if isinstance(v, float) else str(v))
        print(f'| {k} | ' + ' | '.join(cells) + ' |')
    sys.stdout.flush()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default=os.path.join(ROOT, 'configs', 'paper-28nm.json'))
    ap.add_argument('--n', type=int, default=65536)
    ap.add_argument('--study', default='seq,vmt,ports,vru,lanes')
    a = ap.parse_args()
    studies = a.study.split(',')
    stream = ['vvadd', 'daxpy', 'saxpy', 'csaxpy', 'sfilter']
    compute = ['dgemm_opt', 'fma_peak']
    allk = stream + ['gather'] + compute
    if 'seq' in studies:
        table('序列器条目数（n_seq_entries）', allk, a.config, a.n,
              [('8', {'n_seq_entries': 8}), ('12', {'n_seq_entries': 12}), ('16', {'n_seq_entries': 16}), ('32', {'n_seq_entries': 32})])
    if 'vmt' in studies:
        table('VMT 条目数 × 主存延迟（周期）', stream + ['gather'], a.config, a.n,
              [('VMT 64 / lat 110', {'n_vmt_entries': 64, 'mem.dram_latency': 110}),
               ('VMT 32 / lat 110', {'n_vmt_entries': 32, 'mem.dram_latency': 110}),
               ('VMT 16 / lat 110', {'n_vmt_entries': 16, 'mem.dram_latency': 110}),
               ('VMT 64 / lat 220', {'n_vmt_entries': 64, 'mem.dram_latency': 220}),
               ('VMT 128 / lat 220', {'n_vmt_entries': 128, 'mem.dram_latency': 220})])
    if 'ports' in studies:
        table('多 lane 的 L2 端口：1 个 bank（RTL 集成方式）vs 每 lane 一个 bank（论文方式）', stream + ['dgemm_opt'], a.config, a.n,
              [('1 lane / 1 bank', {'n_lanes': 1, 'mem.l2_banks': 1, 'mem.l2_bytes_per_bank': 1048576}),
               ('2 lane / 1 bank', {'n_lanes': 2, 'mem.l2_banks': 1, 'mem.l2_bytes_per_bank': 1048576}),
               ('2 lane / 2 bank', {'n_lanes': 2, 'mem.l2_banks': 2, 'mem.l2_bytes_per_bank': 524288}),
               ('4 lane / 1 bank', {'n_lanes': 4, 'mem.l2_banks': 1, 'mem.l2_bytes_per_bank': 1048576}),
               ('4 lane / 4 bank', {'n_lanes': 4, 'mem.l2_banks': 4, 'mem.l2_bytes_per_bank': 262144})])
    if 'vru' in studies:
        table('VRU（向量运行前预取）', stream + ['gather', 'dgemm_opt'], a.config, a.n,
              [('VRU off', {'build_vru': False}), ('VRU on', {'build_vru': True}),
               ('VRU off, lat 220', {'build_vru': False, 'mem.dram_latency': 220}), ('VRU on, lat 220', {'build_vru': True, 'mem.dram_latency': 220})])
    if 'lanes' in studies:
        table('lane 数（4 bank L2、VRU 开）：周期数', stream + ['gather', 'dgemm_opt'], a.config, a.n,
              [('1', {'n_lanes': 1}), ('2', {'n_lanes': 2}), ('4', {'n_lanes': 4})])
        table('lane 数：GFLOPS（fma_peak 按固定 32 次 vf 计，元素数随 lane 数翻倍）', ['dgemm_opt', 'fma_peak', 'saxpy', 'sfilter'], a.config, a.n,
              [('1', {'n_lanes': 1}), ('2', {'n_lanes': 2}), ('4', {'n_lanes': 4})], metric='gflops')

if __name__ == '__main__':
    main()
