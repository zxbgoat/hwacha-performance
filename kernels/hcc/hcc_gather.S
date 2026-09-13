# @kernel hcc_gather
# @n 4096
# @cfg v64=2 vp=1
# @array idx elem=4 n=4096
# @array out elem=4 n=4096
# @va va0 = idx
# @va va1 = out
# @vs 2
# hwacha-cc 编译 OpenCL 内核得到的向量取指块（~/hwacha-compiler/hwacha-cc/test/bench/bench.s），原样复制；
# 寄存器用量取自该二进制在 Spike 上的 VSETCFG 记录，执行驱动模式下 vl 与地址来自踪迹。
gather_wt:
    vpset vp0
    vlw vv1, va0
    vsll vv0, vv1, vs62
    vlxw vv1, vs63, vv0        # @gather x random
    vsw vv1, va1
    vstop
