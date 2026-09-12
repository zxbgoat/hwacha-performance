from hwacha_perf.isa import parse_instr
from hwacha_perf.program import parse_kernel
from hwacha_perf.hvl import VCfg, max_vlen
from hwacha_perf.config import HwachaConfig


def test_parse_kinds():
    cases = {
        'vld vv0, va0': ('load', True, 0, 3, 'unit'),
        '!vp0 vlw vv1, (va1)': ('load', True, 0, 3, 'unit'),
        'vlstd vv3, va0, va1': ('load', True, 0, 3, 'stride'),
        'vlxw vv4, vs2, vv5': ('load', True, 1, 3, 'indexed'),
        'vsw vv0, (va2)': ('store', True, 1, 3, 'unit'),
        'vfmadd.s vv0, vv0, vs1, vv1': ('fma', True, 2, 1, ''),
        'vfmadd.d vv0, vv1, vv2, vv3': ('fma', True, 3, 1, ''),
        'vfadd.d vs1, vs2, vs3': ('sfp', False, 0, 0, ''),
        'vcmpez vp0, vv0': ('cmp', True, 1, 1, ''),
        'vpxorand vp1, vp0, vp2, vp3': ('plu', True, 0, 1, ''),
        'vfirst vs3, vv1': ('rfirst', True, 1, 1, ''),
        'vamoadd.d vv1, (vv2), vs4': ('amo', True, 1, 4, 'indexed'),
        '@s vaddi vs2, vs1, 4': ('scalar', False, 0, 0, ''),
        'vlad vs5, va3': ('sload', False, 0, 0, ''),
        'vcjal.any vs9, loop': ('branch', True, 0, 1, ''),
        'vstop': ('stop', False, 0, 0, ''),
    }
    for text, (kind, vec, rp, slots, mode) in cases.items():
        i = parse_instr(text, 1)
        assert (i.kind, i.is_vector, i.rp, i.slots, i.mode) == (kind, vec, rp, slots, mode), text


def test_predicate_and_annotation():
    i = parse_instr('@!vp3 vfmadd.d vv0, vv1, vv2, vv3  # @taken 2', 5)
    assert i.pred == 'vp3' and i.pred_neg and i.annot == {'taken': '2'}
    assert 'vp3' in i.reads


def test_hvl():
    cfg = HwachaConfig()
    assert max_vlen(cfg, VCfg(1, 0, 0, 0)) == 2048
    assert max_vlen(cfg, VCfg(2, 0, 0, 0)) == 1024
    assert max_vlen(cfg, VCfg(6, 0, 0, 0)) == 336          # 256//6 = 42 行 × 8
    assert max_vlen(cfg, VCfg(1, 0, 0, 16)) == 128         # 谓词寄存器堆限制 256//16 = 16 行
    assert max_vlen(cfg, VCfg(256, 0, 0, 0)) == 8          # 至少 8
    cfg4 = HwachaConfig(n_lanes=4)
    assert max_vlen(cfg4, VCfg(2, 0, 0, 0)) == 4096
    cfgp = HwachaConfig(conf_prec=True)
    assert max_vlen(cfgp, VCfg(0, 2, 0, 0)) == 2048        # 两个 32 位寄存器 = 一行


def test_kernel_parse():
    src = """
# @kernel t
# @n 100
# @cfg v64=2 vp=1
# @array x elem=8 n=100
# @va va0 = x
# @va va1 = x offset=1 fixed
# @va va2 = 64
# @vs 2
# @ctrl 9
blk:
    vld vv0, va0
    vld vv1, va1
    vcmpez vp0, vv0
 !vp0 vfadd.d vv0, vv0, vv1
    vsd vv0, va0
    vstop
"""
    k = parse_kernel(src)
    assert k.name == 't' and k.n == 100 and k.vcfg == VCfg(2, 0, 0, 1)
    assert k.va['va1'].offset_elems == 1 and not k.va['va1'].advance
    assert k.va['va2'].kind == 'const' and k.va['va2'].value == 64
    assert k.n_vmcs == 2 and k.ctrl_cycles == 9
    assert len(k.instrs) == 6 and k.labels['blk'] == 0
