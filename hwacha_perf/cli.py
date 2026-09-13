"""命令行：
  hwacha-perf run <kernel.S> [--n N] [--lanes L] [--config cfg.json] [--set key=val ...] [--json]
  hwacha-perf analyze <kernel.S> [...]
  hwacha-perf sweep <kernel.S> --lanes 1,2,4 [--vru on,off] [--prec on,off]
  hwacha-perf show-config
"""
from __future__ import annotations
import argparse
import json
import sys
from .config import load_configs, dump_configs
from .program import load_kernel
from .sim import Simulator
from .analytic import analyze
from .trace import load_trace


def _parse_sets(items: list[str]) -> tuple[dict, dict]:
    hw, mem = {}, {}
    for it in items or []:
        k, v = it.split('=', 1)
        v = v.strip()
        if v.lower() in ('true', 'on', 'yes'):
            val = True
        elif v.lower() in ('false', 'off', 'no'):
            val = False
        else:
            try:
                val = int(v, 0)
            except ValueError:
                val = float(v)
        if k.startswith('mem.'):
            mem[k[4:]] = val
        else:
            hw[k] = val
    return hw, mem


def _configs(args):
    hw, mem = _parse_sets(getattr(args, 'set', None))
    if getattr(args, 'lanes', None):
        hw['n_lanes'] = int(args.lanes)
    return load_configs(getattr(args, 'config', None), hw, mem)


def _trace_arg(args):
    if not getattr(args, 'trace', None):
        return None
    lo, hi = 0, 1 << 64
    if args.trace_range:
        lo, hi = (int(x, 16) for x in args.trace_range.split(':'))
    blocks = tuple(int(x) for x in args.trace_blocks.split(':')) if args.trace_blocks else None
    t = load_trace(args.trace, lo, hi, blocks)
    if not t:
        raise SystemExit('trace contains no vf blocks in range')
    return t


def cmd_run(args):
    k = load_kernel(args.kernel)
    cfg, mcfg = _configs(args)
    sim = Simulator(k, cfg, mcfg, n=args.n, trace=_trace_arg(args))
    st = sim.run()
    if args.json:
        print(json.dumps(st.to_dict(), indent=2))
    else:
        print(f"kernel: {k.name}   lanes={cfg.n_lanes} vru={cfg.build_vru} conf_prec={cfg.conf_prec}")
        print(st.report())
        if args.bounds:
            print("\nanalytic bounds:")
            print(analyze(k, cfg, mcfg, n=args.n).report())


def cmd_analyze(args):
    k = load_kernel(args.kernel)
    cfg, mcfg = _configs(args)
    b = analyze(k, cfg, mcfg, n=args.n)
    if args.json:
        print(json.dumps({'maxvl': b.maxvl, 'iterations': b.iterations, 'per_iter': b.per_iter,
                          'binding': b.binding, 'lower_bound_cycles': b.lower_bound_cycles}, indent=2))
    else:
        print(f"kernel: {k.name}   lanes={cfg.n_lanes}")
        print(b.report())


def cmd_sweep(args):
    k = load_kernel(args.kernel)
    lanes = [int(x) for x in args.lanes.split(',')] if args.lanes else [1]
    vrus = [x.strip().lower() in ('on', 'true', '1') for x in args.vru.split(',')] if args.vru else [None]
    precs = [x.strip().lower() in ('on', 'true', '1') for x in args.prec.split(',')] if args.prec else [None]
    rows = []
    print(f"{'lanes':>5} {'vru':>5} {'prec':>5} {'cycles':>10} {'cyc/elem':>9} {'GFLOPS':>8} {'FMA%':>6} {'B/cyc':>7} {'bound':>10}")
    for L in lanes:
        for v in vrus:
            for p in precs:
                hw, mem = _parse_sets(args.set)
                hw['n_lanes'] = L
                if v is not None:
                    hw['build_vru'] = v
                if p is not None:
                    hw['conf_prec'] = p
                cfg, mcfg = load_configs(args.config, hw, mem)
                st = Simulator(k, cfg, mcfg, n=args.n).run()
                b = analyze(k, cfg, mcfg, n=args.n)
                rows.append((L, v, p, st))
                print(f"{L:5d} {str(cfg.build_vru):>5} {str(cfg.conf_prec):>5} {st.cycles:10d} {st.cycles_per_elem:9.3f} "
                      f"{st.gflops:8.2f} {100 * st.fma_util:6.1f} {st.mem_bw_bytes_per_cycle:7.2f} {b.lower_bound_cycles:10d}")
    return rows


def cmd_show_config(args):
    cfg, mcfg = _configs(args)
    print(json.dumps(dump_configs(cfg, mcfg), indent=2))


def main(argv=None):
    ap = argparse.ArgumentParser(prog='hwacha-perf', description='Hwacha 性能模型')
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p):
        p.add_argument('--n', type=int, default=None, help='应用向量长度（覆盖内核文件的 @n）')
        p.add_argument('--lanes', type=int, default=None)
        p.add_argument('--config', default=None, help='JSON 配置文件')
        p.add_argument('--set', action='append', default=[], metavar='KEY=VAL',
                       help='覆盖参数，例如 n_seq_entries=16 或 mem.dram_latency=80')
        p.add_argument('--json', action='store_true')
        p.add_argument('--trace', default=None, help='Spike 指令级踪迹（scripts/hwacha_trace.py run）')
        p.add_argument('--trace-range', default=None, help='vf 块起始 pc 范围 lo:hi（十六进制）')
        p.add_argument('--trace-blocks', default=None, help='只用第 a..b 个块，a:b')

    p = sub.add_parser('run'); p.add_argument('kernel'); common(p)
    p.add_argument('--bounds', action='store_true', help='同时打印解析式下界')
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser('analyze'); p.add_argument('kernel'); common(p); p.set_defaults(fn=cmd_analyze)
    p = sub.add_parser('sweep'); p.add_argument('kernel')
    p.add_argument('--n', type=int, default=None)
    p.add_argument('--lanes', default='1,2,4')
    p.add_argument('--vru', default=None, help='例如 on,off')
    p.add_argument('--prec', default=None, help='例如 on,off')
    p.add_argument('--config', default=None)
    p.add_argument('--set', action='append', default=[])
    p.set_defaults(fn=cmd_sweep)
    p = sub.add_parser('show-config'); common(p); p.set_defaults(fn=cmd_show_config)

    args = ap.parse_args(argv)
    args.fn(args)


if __name__ == '__main__':
    main()
