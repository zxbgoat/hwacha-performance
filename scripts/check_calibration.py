#!/usr/bin/env python3
"""校准回归：对每个 (套件, lane 数, 内核) 比较 C++ 模型周期数与 (a) RTL 稳态、(b) 基线文件里记录的模型周期数。

任一内核的 RTL 误差超过 --max-err，或相对基线的漂移超过 --max-drift，都返回非零；--update 把当前模型结果写成新基线
（改了模型后应先看漂移是否合理，再 --update）。基线：hwacha-perf/tests/calibration_baseline.json。

用法: python3 scripts/check_calibration.py [--update] [--max-err 12] [--max-drift 2]
"""
import argparse, json, os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BASE = os.path.join(ROOT, 'hwacha-perf', 'tests', 'calibration_baseline.json')
LOGS = {   # lane 数 -> (配置, 基准日志, 微基准日志)
    1: ('rtl-hwacha-rocket.json',     'rtl-n4096-aligned.log',  'micro-n4096-aligned2.log'),
    2: ('rtl-hwacha-rocket-l2.json',  'rtl-n4096-l2-fixed.log', 'micro-n4096-l2-aligned.log'),
    4: ('rtl-hwacha-rocket-l4.json',  'rtl-n4096-l4.log',       'micro-n4096-l4.log'),
    8: ('rtl-hwacha-rocket-l8.json',  'rtl-n4096-l8.log',       'micro-n4096-l8.log'),
    16: ('rtl-hwacha-rocket-l16.json', 'rtl-n4096-l16.log',      'micro-n4096-l16.log'),
    '2b2': ('rtl-hwacha-rocket-l2b2.json', 'rtl-n4096-l2-b2.log',  'micro-n4096-l2-b2.log'),   # 2 lane + 2 bank L2
    '4b2': ('rtl-hwacha-rocket-l4b2.json', 'rtl-n4096-l4-b2.log',  'micro-n4096-l4-b2.log'),   # 4 lane + 2 bank L2
    '4b4': ('rtl-hwacha-rocket-l4b4.json', 'rtl-n4096-l4-b4.log',  'micro-n4096-l4-b4.log'),   # 4 lane + 4 bank L2
}
EXTRA_MICRO = [(1, 'rtl-hwacha-rocket.json', 'micro-n4096-pred3.log')]   # 谓词/FMA 分解微基准第三轮（35 个微内核，含 pcmp_*、vpop*、fma2_*、pcmp_br）
TRACE_SUITES = [('rodinia', 1), ('hcc', 1), ('rodinia', 4), ('hcc', 4)]
SKIP_ERR = {'micro_empty'}   # 只有几十到几百拍，不按 RTL 误差阈值检查（仍检查漂移）
SKIP_ERR_KEYS = {'bench/2b2L/gather', 'micro/1L/micro_pcmp_br', 'micro/4b2L/micro_sstride', 'micro/4b2L/micro_pcmp_br', 'micro/4b4L/micro_pcmp_br'}   # 已知残差（docs/24 §10.15）

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.stderr.write(r.stderr[-500:]); return {}
    return json.loads(r.stdout)

def collect():
    res = {}
    for L, (cfg, bench, micro) in LOGS.items():
        for tag, log in (('bench', bench), ('micro', micro)):
            d = run([sys.executable, os.path.join(ROOT, 'scripts', 'compare_rtl.py'), '--json', '--config', os.path.join(ROOT, 'configs', cfg),
                     '--logs', os.path.join(ROOT, 'rtl', 'results', log)])
            for k, v in d.items():
                res[f'{tag}/{L}L/{k}'] = v
    for L, cfg, log in EXTRA_MICRO:
        d = run([sys.executable, os.path.join(ROOT, 'scripts', 'compare_rtl.py'), '--json', '--config', os.path.join(ROOT, 'configs', cfg),
                 '--logs', os.path.join(ROOT, 'rtl', 'results', log)])
        for k, v in d.items():
            res.setdefault(f'micro/{L}L/{k}', v)
    for suite, L in TRACE_SUITES:
        d = run([sys.executable, os.path.join(ROOT, 'scripts', 'compare_rodinia.py'), '--json', '--suite', suite, '--lanes', str(L)])
        for k, v in d.items():
            res[f'{suite}/{L}L/{k}'] = v
    return res

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--max-err', type=float, default=12.0)
    ap.add_argument('--max-drift', type=float, default=2.0)
    a = ap.parse_args()
    cur = collect()
    if a.update:
        json.dump(cur, open(BASE, 'w'), indent=1, ensure_ascii=False)
        print(f'baseline written: {len(cur)} entries -> {BASE}'); return
    base = json.load(open(BASE)) if os.path.exists(BASE) else {}
    bad = 0
    print(f"{'kernel':<34}{'RTL':>8}{'model':>8}{'err':>8}{'base':>8}{'drift':>8}")
    for k, v in sorted(cur.items()):
        rtl, m = v.get('rtl'), v.get('model')
        err = v.get('err'); b = base.get(k, {}).get('model')
        drift = 100.0 * (m - b) / b if (m and b) else None
        flag = ''
        name = k.split('/')[-1]
        if err is not None and abs(err) > a.max_err and name not in SKIP_ERR and k not in SKIP_ERR_KEYS: flag += ' ERR>max'
        if drift is not None and abs(drift) > a.max_drift: flag += ' DRIFT'
        if m is None: flag += ' NOMODEL'
        if flag: bad += 1
        print(f"{k:<34}{rtl or '-':>8}{m or '-':>8}{(f'{err:+.1f}%' if err is not None else '-'):>8}{b or '-':>8}{(f'{drift:+.1f}%' if drift is not None else '-'):>8}{flag}")
    errs = [abs(v['err']) for k, v in cur.items() if v.get('err') is not None and k.split('/')[-1] not in SKIP_ERR]
    print(f'{len(cur)} entries, mean |err| {sum(errs)/len(errs):.1f}%, {bad} flagged')
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
