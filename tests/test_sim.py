import os
import pytest
from hwacha_perf import HwachaConfig, MemoryConfig, load_kernel, Simulator, analyze

K = os.path.join(os.path.dirname(__file__), '..', 'kernels')


def run(name, n=4096, **hw):
    k = load_kernel(os.path.join(K, name))
    cfg = HwachaConfig(**hw)
    st = Simulator(k, cfg, MemoryConfig(), n=n).run()
    return k, cfg, st


def test_fma_peak_reaches_dual_fma_throughput():
    k, cfg, st = run('fma_peak.S', n=8192)
    # 2 向量源操作数 → 读口 2 拍/strip；两个 FMA 簇各 4 拍/strip → 每拍 4 个 DP FMA
    assert st.fma_util > 0.97
    assert abs(st.gflops - 8.0) < 0.3
    assert st.load_beats == 0 and st.store_beats == 0


def test_streaming_kernel_is_dram_bound():
    k, cfg, st = run('vvadd.S', n=8192)
    b = analyze(k, cfg, MemoryConfig(), n=8192)
    assert b.binding == 'dram_bandwidth'
    # 模拟结果不低于解析下界，且相差不超过 15%
    assert st.cycles >= b.lower_bound_cycles * 0.98
    assert st.cycles <= b.lower_bound_cycles * 1.15


def test_vru_helps_streaming_kernel():
    _, _, on = run('daxpy.S', n=8192, build_vru=True)
    _, _, off = run('daxpy.S', n=8192, build_vru=False)
    assert on.cycles < off.cycles
    assert on.mem.prefetch_hits > 0
    assert off.mem.prefetch_hits == 0


def test_more_lanes_help_compute_bound_kernel():
    # n 足够大使 B 块（128 KB）在 L2 中被多次复用，内核成为计算受限
    _, _, one = run('dgemm_opt.S', n=32768, n_lanes=1)
    _, _, two = run('dgemm_opt.S', n=32768, n_lanes=2)
    assert one.fma_util > 0.8
    assert two.cycles < one.cycles * 0.7
    assert two.max_vlen == 2 * one.max_vlen


def test_mixed_precision_doubles_single_precision_rate():
    _, _, base = run('saxpy.S', n=8192, conf_prec=False, build_vru=False)
    _, _, mxp = run('saxpy.S', n=8192, conf_prec=True, build_vru=False)
    # 混合精度：两个 32 位寄存器只占一行 → HVL 翻倍，strip 内元素数翻倍
    assert mxp.max_vlen == 2 * base.max_vlen
    assert mxp.strips_issued < base.strips_issued


def test_sequencer_slots_limit_window():
    _, _, small = run('vvadd.S', n=4096, n_seq_entries=8, build_vru=False)
    _, _, big = run('vvadd.S', n=4096, n_seq_entries=16, build_vru=False)
    assert small.scalar_cycle.get('seq_full', 0) > big.scalar_cycle.get('seq_full', 0)
    assert big.cycles <= small.cycles


def test_predicated_kernel_runs():
    k, cfg, st = run('csaxpy.S', n=4096)
    assert st.fma_elems == 4096
    assert st.cycles > 0 and not st.warnings


def test_gather_uses_one_beat_per_element():
    k, cfg, st = run('gather.S', n=2048)
    # vld idx: 2048×8B/16 = 1024 beats；gather 2048 beats；store 1024 beats
    # 相邻随机元素偶尔落在同一 beat 会被合并，允许少量偏差
    assert 1024 + 2048 - 16 <= st.load_beats <= 1024 + 2048
    assert st.store_beats == 1024


def test_branch_loop_executes_taken_count():
    src = """
# @kernel br
# @n 512
# @cfg v64=2 vp=1
# @array x elem=8 n=512
# @va va0 = x
    vld vv0, va0
loop:
    vfadd.d vv0, vv0, vs1
    vcmpez vp0, vv0
    vcjal.any vs2, loop   # @taken 3
    vsd vv0, va0
    vstop
"""
    from hwacha_perf.program import parse_kernel
    k = parse_kernel(src)
    st = Simulator(k, HwachaConfig(), MemoryConfig(), n=512).run()
    # 一次 vf：1 load + 4×(fadd,cmp,branch) + store + 分支… vector_ops = 1 + 4*3 + 1 = 14
    assert st.vector_ops == 14
