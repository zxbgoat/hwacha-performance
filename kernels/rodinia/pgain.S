# @kernel pgain
# @n 1024
# @cfg v64=8 vp=9
# @array a0 elem=4 n=1024
# @va va0 = a0
# @array a1 elem=4 n=1024
# @va va1 = a1
# @array a2 elem=4 n=1024
# @va va2 = a2
# @array a3 elem=4 n=1024
# @va va3 = a3
# @array a4 elem=4 n=1024
# @va va4 = a4
# @array a5 elem=4 n=1024
# @va va5 = a5
# @array a6 elem=4 n=1024
# @va va6 = a6
# @array a7 elem=4 n=1024
# @va va7 = a7
# @array a8 elem=4 n=1024
# @va va8 = a8
# @array a9 elem=4 n=1024
# @va va9 = a9
# @vs 17
# 由 scripts/extract_hcc_kernel.py 从 pgain.s 的 pgain_kernel_wt 区域抠出（多入口块按原地址顺序连续排列，
# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。
pgain_kernel_wt:
    vpset vp0
    veidx vv0
    vadd vv0, vv0, vs63
    veidx vv1
    vcmplt vp1, vv0, vs60
    vpop vp2, vp0, vp1, vp1, 0x80
    vpop vp3, vp0, vp1, vp1, 0x02
    @!vp2 vcjal 0, vs1, .Lpgain_kernel_skip0
    vcmpeq vp1, vv1, vs0
    vaddw vv4, vs0, vs0
    vcmplt vp4, vv4, vs59
    vpop vp1, vp1, vp4, vp1, 136
.Lpgain_kernel_skip0:
    vpop vp5, vp2, vp1, vp1, 0x80
    vpop vp6, vp2, vp1, vp1, 0x02
    @!vp5 vcjal 0, vs1, .Lpgain_kernel_skip1
    vmul vs3, vs57, vs48
    vadd vs2, vs58, vs3
    vsll vs3, vs59, vs51
    vsrl vs3, vs3, vs51
.Lpgain_kernel_skip1:
    vpop vp1, vp5, vp5, vp5, 0xAA
    @vp5 vadd vs4, vs0, vs0
    vpclear vp2
.Lpgain_kernel_loop2:
    vmul vs5, vs4, vs60
    vmul vs6, vs5, vs48
    vadd vs5, vs2, vs6
    vlsw vs5, vs5
    vmul vs7, vs4, vs48
    vadd vs6, vs56, vs7
    vssw vs6, vs5
    vadd vs4, vs4, vs55
    vadd vv2, vs4, vs0
    vcmpeq vp5, vv2, vs3
    vpop vp7, vp1, vp5, vp5, 0x80
    vpop vp8, vp1, vp5, vp5, 0x02
    vpop vp2, vp2, vp7, vp7, 0xEE
    vpop vp1, vp1, vp7, vp7, 0x02
    @vp8 vcjal 1, vs1, .Lpgain_kernel_loop2
    vpop vp1, vp2, vp2, vp2, 0xAA
    vpop vp1, vp1, vp6, vp6, 0xEE
    vfence
    vpop vp2, vp1, vp4, vp4, 0x80
    vpop vp5, vp1, vp4, vp4, 0x02
    @!vp2 vcjal 0, vs1, .Lpgain_kernel_skip3
    vsll vs2, vs59, vs51
    vsrl vs2, vs2, vs51
.Lpgain_kernel_skip3:
    @vp2 vaddw vv4, vs0, vs0
    vstop
pgain_kernel_wt_r0_b0:
    @vp2 vlw vv5, va2
    @vp2 vfsub.s vv5, vv5, vs3
    @vp2 vfmadd.s vv4, vv5, vv5, vv4
    vstop
pgain_kernel_wt_a0:
    vpop vp1, vp2, vp2, vp2, 0xAA
    vpop vp1, vp1, vp5, vp5, 0xEE
    @vp5 vaddw vv5, vs0, vs0
    @vp2 vaddw vv5, vv4, vs0
    @!vp1 vcjal 0, vs1, .Lpgain_kernel_skip4
    @vp1 vlstw vv4, va0, va6
    @vp1 vfmul.s vv5, vv5, vv4
    @vp1 vlstw vv4, va1, va7
    vaddw vs2, vs53, vs54
    @vp1 vmul vv2, vs2, vv0
    vcmpflt.s vp2, vv5, vv4
.Lpgain_kernel_skip4:
    vpop vp4, vp1, vp2, vp2, 0x80
    vpop vp5, vp1, vp2, vp2, 0x02
    @!vp4 vcjal 0, vs1, .Lpgain_kernel_skip5
    vaddw vv6, vs52, vs0
    vlb vv7, va3
    @vp4 vaddw vv7, vv6, vs0
    vsb vv7, va3
    @vp4 vfsub.s vv6, vv5, vv4
    @vp4 vsstw vv6, va4, va8
.Lpgain_kernel_skip5:
    @!vp5 vcjal 0, vs1, .Lpgain_kernel_skip6
    @vp5 vlstd vv3, va5, va9
    @vp5 vsll vv3, vv3, vs51
    @vp5 vsra vv3, vv3, vs50
    @vp5 vlxw vv6, vs62, vv3
    @vp5 vadd vv2, vv6, vv2
    @vp5 vfsub.s vv5, vv4, vv5
    @vp5 vsll vv2, vv2, vs49
    @vp5 vlxw vv4, vs61, vv2
    @vp5 vfadd.s vv5, vv5, vv4
    @vp5 vsxw vv5, vs61, vv2
.Lpgain_kernel_skip6:
    vpop vp1, vp5, vp5, vp5, 0xAA
    vpop vp1, vp1, vp4, vp4, 0xEE
    vpop vp1, vp1, vp3, vp3, 0xEE
    vstop

# ---- control threads (from llc) ----
	.file	"hwacha-ct"
