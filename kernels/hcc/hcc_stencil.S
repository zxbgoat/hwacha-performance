# @kernel hcc_stencil
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=4 n=4096
# @array out elem=4 n=4096
# @va va0 = x
# @va va1 = x offset=1
# @va va2 = x offset=2
# @va va3 = out
# @vs 2
# hwacha-cc 编译 OpenCL 内核得到的向量取指块（~/hwacha-compiler/hwacha-cc/test/bench/bench.s），原样复制；
# 寄存器用量取自该二进制在 Spike 上的 VSETCFG 记录，执行驱动模式下 vl 与地址来自踪迹。
stencil_wt:
    vpset vp0
    vlw vv0, va0
    vlw vv1, va1
    vfmul.s vv1, vv1, vs63
    vfmadd.s vv1, vv0, vs62, vv1
    vlw vv0, va2
    vfmadd.s vv1, vv0, vs62, vv1
    vsw vv1, va3
    vstop
