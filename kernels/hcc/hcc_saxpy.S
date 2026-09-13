# @kernel hcc_saxpy
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=4 n=4096
# @array y elem=4 n=4096
# @va va0 = x
# @va va1 = y
# @vs 1
# hwacha-cc 编译 OpenCL 内核得到的向量取指块（~/hwacha-compiler/hwacha-cc/test/bench/bench.s），原样复制；
# 寄存器用量取自该二进制在 Spike 上的 VSETCFG 记录，执行驱动模式下 vl 与地址来自踪迹。
saxpy_wt:
    vpset vp0
    vlw vv0, va0
    vlw vv1, va1
    vfmadd.s vv1, vs63, vv0, vv1
    vsw vv1, va1
    vstop
