#!/usr/bin/env python3
"""把 rtl/results/*.log 中的 RESULT 行与 C++ 模型（RTL 配置）逐内核比较。

用法: python3 scripts/compare_rtl.py [--logs rtl/results/rtl-n4096-aligned.log,...] [--config configs/rtl-hwacha-rocket.json]
"""
import argparse, glob, json, os, re, subprocess, sys
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from logio import openlog, resolve
ROOT = os.path.join(os.path.dirname(__file__), '..')

def rtl_results(paths):
    out = {}
    for p in paths:
        for line in openlog(p):
            m = re.match(r'RESULT (\S+) (\d+) cycles (\d+) elems', line)
            if m:
                out[(m.group(1), int(m.group(3)))] = int(m.group(2))
    return out

def _sets(sets):
    out = []
    for kv in sets or []:
        out += ['--set', kv]
    return out

def run_cc(kernel, n, cfg, sets=None):
    exe = os.path.join(ROOT, 'hwacha-perf', 'build', 'hwacha-sim')
    r = subprocess.run([exe, 'run', kernel, '--n', str(n), '--config', cfg, '--quiet', '--json'] + _sets(sets), capture_output=True, text=True)
    try:
        return json.loads(r.stdout)['cycles']
    except Exception:
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logs', default=None)
    ap.add_argument('--config', default=os.path.join(ROOT, 'configs', 'rtl-hwacha-rocket.json'))
    ap.add_argument('--no-py', action='store_true', help='（已无作用，Python 模型已删除）')
    ap.add_argument('--set', action='append', default=None, help='传给模型的参数覆盖（如 mem.l2_store_switch=0）')
    ap.add_argument('--json', action='store_true', help='输出 {kernel: {rtl, model, err}} 的 JSON')
    ap.add_argument('--cold', action='store_true', help='与 RTL 的第一次（冷）计时比较，模型打开 mem.cold_start')
    ap.add_argument('--max-err', type=float, default=None, help='任一内核 C++ 误差超过该百分比则返回非零')
    a = ap.parse_args()
    logs = a.logs.split(',') if a.logs else sorted(glob.glob(os.path.join(ROOT, 'rtl', 'results', '*.log')))
    res = rtl_results(logs)
    # 模型对应稳态（数据驻留 L2、无 L1D 脏行、VI$ 已热），优先与 *_warm 结果比较，同时列出冷启动结果
    out = {}
    if not a.json:
        print(f"{'kernel':<14}{'N':>7}{'RTL cold':>10}{'RTL steady':>11}{'C++':>9}{'err':>8}")
    errs = []
    for (k, n), rtl in sorted(res.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if k.endswith('_warm') or k.endswith('_warm2'):
            continue
        kp = os.path.join(ROOT, 'kernels', k + '.S')
        if not os.path.exists(kp):
            continue
        warm = res.get((k + '_warm2', n)) or res.get((k + '_warm', n))   # 优先无标量验证循环干扰的第三次计时
        ref = rtl if a.cold else (warm if warm else rtl)
        c = run_cc(kp, n, a.config, (a.set or []) + (['mem.cold_start=true'] if a.cold else []))
        def e(v): return f"{100*(v-ref)/ref:+.1f}%" if v else 'n/a'
        out[k] = {'rtl': ref, 'model': c, 'err': (100*(c-ref)/ref) if c else None}
        if not a.json:
            print(f"{k:<14}{n:>7}{rtl:>10}{warm if warm else '-':>11}{c if c else 'ERR':>9}{e(c):>8}")
        if c: errs.append(abs(100*(c-ref)/ref))
    if a.json:
        print(json.dumps(out, indent=1)); return
    if errs:
        print(f"C++ mean |err| = {sum(errs)/len(errs):.1f}%, max = {max(errs):.1f}%")
        if a.max_err is not None and max(errs) > a.max_err:
            print(f"FAIL: max error {max(errs):.1f}% > {a.max_err}%")
            sys.exit(1)

if __name__ == '__main__':
    main()
