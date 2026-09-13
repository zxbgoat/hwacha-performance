#!/usr/bin/env python3
"""从 hwacha-cc 生成的 .s 文件里抠出一个内核的全部工作线程块（含 _wt_r*_b*、_wt_a* 等多入口块），
加上性能模型需要的注解，写成 kernels/…/<name>.S（只用于执行驱动模式：vl、分支、地址都来自 Spike 踪迹）。

用法: extract_hcc_kernel.py <file.s> <wt_symbol> <name> --cfg v64=8 vp=6 [--n 1024] [--vs K] [-o out.S]
"""
import argparse, re, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('symbol'); ap.add_argument('name')
    ap.add_argument('--cfg', nargs='+', required=True); ap.add_argument('--n', type=int, default=1024)
    ap.add_argument('--vs', type=int, default=None); ap.add_argument('-o', default=None)
    a = ap.parse_args()
    lines = open(a.src).read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.strip() == a.symbol + ':')
    # 区域到下一个不属于本内核的全局符号（另一个 *_wt 或 *_ct）为止
    end = len(lines)
    for i in range(start + 1, len(lines)):
        m = re.match(r'^([A-Za-z_]\w*):', lines[i])
        if m and not m.group(1).startswith(a.symbol) and not m.group(1).startswith('.L'):
            end = i; break
    body = [l for l in lines[start:end] if not l.strip().startswith(('.text', '.align', '.globl', '.type', '.size', '.cfi', '.p2align'))]
    # 去掉块尾的空行
    while body and not body[-1].strip(): body.pop()
    text = '\n'.join(body)
    vas = sorted(set(re.findall(r'\bva(\d+)\b', text)), key=int)
    vmcs = a.vs if a.vs is not None else len(set(re.findall(r'vmcs\s+(vs\d+)', '\n'.join(lines))))
    hdr = [f'# @kernel {a.name}', f'# @n {a.n}', '# @cfg ' + ' '.join(a.cfg)]
    for v in vas:
        hdr.append(f'# @array a{v} elem=4 n={a.n}')
        hdr.append(f'# @va va{v} = a{v}')
    hdr.append(f'# @vs {vmcs}')
    hdr.append(f'# 由 scripts/extract_hcc_kernel.py 从 {a.src.split("/")[-1]} 的 {a.symbol} 区域抠出（多入口块按原地址顺序连续排列，')
    hdr.append('# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。')
    out = '\n'.join(hdr) + '\n' + text + '\n'
    path = a.o or f'kernels/rodinia/{a.name}.S'
    open(path, 'w').write(out)
    n_ins = sum(1 for l in body if l.strip() and not l.strip().startswith('#') and not re.match(r'^\S+:', l.strip()))
    print(f'{path}: {n_ins} instrs, va={vas}, vmcs={vmcs}')

if __name__ == '__main__':
    main()
