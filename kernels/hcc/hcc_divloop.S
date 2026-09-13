# @kernel hcc_divloop
# @n 4096
# @cfg v64=3 vp=7
# @array cnt elem=4 n=4096
# @array out elem=4 n=4096
# @va va0 = cnt
# @va va1 = out
# @vs 4
# hwacha-cc 编译 OpenCL 内核得到的向量取指块（~/hwacha-compiler/hwacha-cc/test/bench/bench.s），原样复制；
# 寄存器用量取自该二进制在 Spike 上的 VSETCFG 记录，执行驱动模式下 vl 与地址来自踪迹。
divloop_wt:
    vpset vp0
    vlw vv1, va0
    vcmplt vp1, vs0, vv1
    vpop vp2, vp0, vp1, vp1, 0x80
    vpop vp3, vp0, vp1, vp1, 0x02
    @!vp2 vcjal 0, vs1, .Ldivloop_skip0
    vsll vv0, vv1, vs61
    vsrl vv0, vv0, vs61
.Ldivloop_skip0:
    vpop vp1, vp2, vp2, vp2, 0xAA
    @vp2 vadd vs2, vs0, vs0
    @vp2 vaddw vv1, vs0, vs0
    vpclear vp4
.Ldivloop_loop1:
    vmul vs4, vs2, vs60
    vadd vs3, vs63, vs4
    vlsw vs3, vs3
    @vp1 vfadd.s vv1, vv1, vs3
    vadd vs2, vs2, vs62
    vcmpeq vp2, vs2, vv0
    vpop vp5, vp1, vp2, vp2, 0x80
    vpop vp6, vp1, vp2, vp2, 0x02
    @vp5 vaddw vv2, vv1, vs0
    vpop vp4, vp4, vp5, vp5, 0xEE
    vpop vp1, vp1, vp5, vp5, 0x02
    @vp6 vcjal 1, vs1, .Ldivloop_loop1
    vpop vp1, vp4, vp4, vp4, 0xAA
    vpop vp1, vp1, vp3, vp3, 0xEE
    @vp3 vaddw vv1, vs0, vs0
    @vp4 vaddw vv1, vv2, vs0
    @vp1 vsw vv1, va1
    vstop
