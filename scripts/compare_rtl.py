#!/usr/bin/env python3
"""把 rtl/results/*.log 中的 RESULT 行与 C++ 模型（RTL 配置）逐内核比较。

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

def _sets(sets):
    out = []
    for kv in sets or []:
        out += ['--set', kv]
    return out

def run_cc(kernel, n, cfg, sets=None):
    exe = os.path.join(ROOT, 'cc', 'build', 'hwacha-sim')
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
    ap.add_argument('--set', action='append', default=None, help='传给两个模型的参数覆盖（如 mem.rocc_shared_port=false）')
    ap.add_argument('--max-err', type=float, default=None, help='任一内核 C++ 误差超过该百分比则返回非零')
    a = ap.parse_args()
    logs = a.logs.split(',') if a.logs else sorted(glob.glob(os.path.join(ROOT, 'rtl', 'results', '*.log')))
    res = rtl_results(logs)
    # 模型对应稳态（数据驻留 L2、无 L1D 脏行、VI$ 已热），优先与 *_warm 结果比较，同时列出冷启动结果
    print(f"{'kernel':<14}{'N':>7}{'RTL cold':>10}{'RTL steady':>11}{'C++':>9}{'err':>8}")
    errs = []
    for (k, n), rtl in sorted(res.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if k.endswith('_warm') or k.endswith('_warm2'):
            continue
        kp = os.path.join(ROOT, 'kernels', k + '.S')
        if not os.path.exists(kp):
            continue
        warm = res.get((k + '_warm2', n)) or res.get((k + '_warm', n))   # 优先无标量验证循环干扰的第三次计时
        ref = warm if warm else rtl
        c = run_cc(kp, n, a.config, a.set)
        def e(v): return f"{100*(v-ref)/ref:+.1f}%" if v else 'n/a'
        print(f"{k:<14}{n:>7}{rtl:>10}{warm if warm else '-':>11}{c if c else 'ERR':>9}{e(c):>8}")
        if c: errs.append(abs(100*(c-ref)/ref))
    if errs:
        print(f"C++ mean |err| = {sum(errs)/len(errs):.1f}%, max = {max(errs):.1f}%")
        if a.max_err is not None and max(errs) > a.max_err:
            print(f"FAIL: max error {max(errs):.1f}% > {a.max_err}%")
            sys.exit(1)

if __name__ == '__main__':
    main()
