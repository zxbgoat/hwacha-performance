#!/usr/bin/env python3
"""统计 RTL 的 TileLink 通道跟踪（+verbose +hwacha_tl_trace=1，rtl/results/tlv-*.log）：
按 RESULT 行切段，给出每段 VMU A 通道请求数、平均/最大等待拍数、发射间隔分布、D 通道等待、L2 内侧等待与 MSHR 占用。

用法: python3 scripts/tl_trace_stats.py rtl/results/tlv-micro-n4096.log [--gaps]
"""
import argparse, re, collections
A_RE = re.compile(r'^HTL A cyc=\s*(\d+) op=\s*(\d+) addr=\s*([0-9a-f]+) src=\s*(\d+) wait=\s*(\d+)')
D_RE = re.compile(r'^HTL D cyc=\s*(\d+) op=\s*(\d+) src=\s*(\d+) wait=\s*(\d+)')
L2A_RE = re.compile(r'^L2 inA cyc=\s*(\d+) op=\s*(\d+) addr=\s*([0-9a-f]+) src=\s*(\d+) size=\s*(\d+) wait=\s*(\d+) mshr=\s*(\d+)')
L2OA_RE = re.compile(r'^L2 outA cyc=\s*(\d+) op=\s*(\d+) addr=\s*([0-9a-f]+)')
L2OC_RE = re.compile(r'^L2 outC cyc=\s*(\d+) op=\s*(\d+) addr=\s*([0-9a-f]+)')

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('log'); ap.add_argument('--gaps', action='store_true')
    ap.add_argument('--seg', default=None, help='只看名字含该子串的段')
    a = ap.parse_args()
    seg = {'A': [], 'D': [], 'L2A': [], 'L2OA': 0, 'L2OC': 0}
    def report(name, s):
        if a.seg and a.seg not in name: return
        A = s['A']
        if not A:
            print(f'{name:<24} (no VMU traffic)'); return
        n = len(A); span = A[-1][0] - A[0][0] + 1
        waits = [w for _, _, _, _, w in A]
        gaps = collections.Counter(A[i][0] - A[i - 1][0] for i in range(1, n))
        ops = collections.Counter(op for _, op, _, _, _ in A)
        srcs = collections.Counter(src for _, _, _, src, _ in A)
        dw = [w for _, _, _, w in s['D']]
        l2w = [w for *_, w, _ in s['L2A']]; l2m = [m for *_, m in s['L2A']]
        opname = {0: 'PutFull', 1: 'PutPart', 2: 'Arith', 3: 'Logic', 4: 'Get'}
        print(f"{name:<24} A={n:5d} span={span:6d} beats/cyc={n/span:5.3f} ops={{{', '.join(f'{opname.get(o,o)}:{c}' for o,c in sorted(ops.items()))}}} "
              f"srcs={len(srcs)} Await mean={sum(waits)/n:5.2f} max={max(waits):3d} stalled={sum(1 for w in waits if w)/n*100:4.1f}% "
              f"Dwait mean={sum(dw)/max(1,len(dw)):5.2f} L2wait mean={sum(l2w)/max(1,len(l2w)):5.2f} mshr mean={sum(l2m)/max(1,len(l2m)):4.2f} "
              f"L2miss={s['L2OA']} wb={s['L2OC']}")
        if a.gaps:
            tot = sum(gaps.values())
            print('    gap histogram: ' + ', '.join(f'{g}:{c/tot*100:.1f}%' for g, c in sorted(gaps.items())[:12]))
    for line in open(a.log, errors='replace'):
        m = A_RE.match(line)
        if m:
            seg['A'].append((int(m[1]), int(m[2]), int(m[3], 16), int(m[4]), int(m[5]))); continue
        m = D_RE.match(line)
        if m:
            seg['D'].append((int(m[1]), int(m[2]), int(m[3]), int(m[4]))); continue
        m = L2A_RE.match(line)
        if m:
            seg['L2A'].append((int(m[1]), int(m[2]), int(m[3], 16), int(m[4]), int(m[5]), int(m[6]), int(m[7]))); continue
        if L2OA_RE.match(line): seg['L2OA'] += 1; continue
        if L2OC_RE.match(line): seg['L2OC'] += 1; continue
        if line.startswith('RESULT'):
            report(line.split()[1], seg)
            seg = {'A': [], 'D': [], 'L2A': [], 'L2OA': 0, 'L2OC': 0}

if __name__ == '__main__':
    main()
