#!/usr/bin/env python3
"""把 rtl/results/*.log 中的 RESULT 行与两个模型（RTL 配置）逐内核比较。

用法: python3 scripts/compare_rtl.py [--logs rtl/results/rtl-n4096.log,...] [--config configs/rtl-hwacha-rocket.json]
"""
import argparse, glob, json, os, re, subprocess, sys
ROOT = os.path.join(os.path.dirname(__file__), '..')

def rtl_results(paths):
    out = {}
    for p in paths:
        for line in open(p, errors='replace'):
            m = re.match(r'RESULT (\S+) (\d+) cycles (\d+) elems', line)
            if m:
                out[(m.group(1), int(m.group(3)))] = int(m.group(2))
    return out

def run_cc(kernel, n, cfg):
    exe = os.path.join(ROOT, 'cc', 'build', 'hwacha-sim')
    r = subprocess.run([exe, 'run', kernel, '--n', str(n), '--config', cfg, '--quiet', '--json'], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)['cycles']
    except Exception:
        return None

def run_py(kernel, n, cfg):
    r = subprocess.run([sys.executable, '-m', 'hwacha_perf.cli', 'run', kernel, '--n', str(n), '--config', cfg, '--json'],
                       capture_output=True, text=True, cwd=ROOT)
    try:
        return json.loads(r.stdout)['cycles']
    except Exception:
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logs', default=None)
    ap.add_argument('--config', default=os.path.join(ROOT, 'configs', 'rtl-hwacha-rocket.json'))
    ap.add_argument('--no-py', action='store_true')
    a = ap.parse_args()
    logs = a.logs.split(',') if a.logs else sorted(glob.glob(os.path.join(ROOT, 'rtl', 'results', '*.log')))
    res = rtl_results(logs)
    # 模型对应稳态（数据驻留 L2、无 L1D 脏行、VI$ 已热），优先与 *_warm 结果比较，同时列出冷启动结果
    print(f"{'kernel':<14}{'N':>7}{'RTL cold':>10}{'RTL warm':>10}{'C++':>9}{'err':>8}{'Python':>9}{'err':>8}")
    errs = []
    for (k, n), rtl in sorted(res.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if k.endswith('_warm'):
            continue
        kp = os.path.join(ROOT, 'kernels', k + '.S')
        if not os.path.exists(kp):
            continue
        warm = res.get((k + '_warm', n))
        ref = warm if warm else rtl
        c = run_cc(kp, n, a.config)
        p = None if a.no_py else run_py(kp, n, a.config)
        def e(v): return f"{100*(v-ref)/ref:+.1f}%" if v else 'n/a'
        print(f"{k:<14}{n:>7}{rtl:>10}{warm if warm else '-':>10}{c if c else 'ERR':>9}{e(c):>8}{p if p else '-':>9}{e(p):>8}")
        if c: errs.append(abs(100*(c-ref)/ref))
    if errs:
        print(f"C++ mean |err| = {sum(errs)/len(errs):.1f}%, max = {max(errs):.1f}%")

if __name__ == '__main__':
    main()
