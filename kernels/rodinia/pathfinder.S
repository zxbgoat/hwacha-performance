# @kernel pathfinder
# @n 1024
# @cfg v64=10 vp=14
# @array a0 elem=4 n=1024
# @va va0 = a0
# @array a1 elem=4 n=1024
# @va va1 = a1
# @vs 19
# 由 scripts/extract_hcc_kernel.py 从 pathfinder.s 的 dynproc_kernel_wt 区域抠出（多入口块按原地址顺序连续排列，
# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。
dynproc_kernel_wt:
    vpset vp0
    veidx vv0
    vpclear vp1
    vsllw vs2, vs57, vs56
    vmulw vs2, vs2, vs55
    vsubw vs2, vs63, vs2
    vmulw vs2, vs2, vs62
    vsubw vs2, vs2, vs54
    vaddw vs3, vs2, vs63
    vadd vv1, vs2, vv0
    vaddw vv6, vs2, vv0
    vaddw vv7, vs2, vs0
    vcmplt vp2, vv7, vs0
    @vp2 vaddw vv8, vv7, vs0
    @!vp2 vaddw vv8, vs0, vs0
    vsubw vv8, vs0, vv8
    vaddw vv7, vs53, vs0
    vcmplt vp2, vv7, vs3
    vaddw vs2, vs63, vs52
    vsubw vs3, vs53, vs3
    @vp2 vaddw vv7, vs3, vs0
    @!vp2 vaddw vv7, vs0, vs0
    vaddw vv7, vs2, vv7
    vaddw vv9, vv0, vs52
    vcmplt vp2, vv9, vv8
    @vp2 vaddw vv9, vv8, vs0
    @!vp2 vaddw vv9, vv9, vs0
    vcmplt vp2, vv0, vv8
    vpop vp2, vp2, vp2, vp2, 0x55
    vcmplt vp3, vv7, vv0
    vpop vp3, vp3, vp3, vp3, 0x55
    vcmplt vp4, vs52, vv6
    vcmplt vp5, vv6, vs53
    vpop vp4, vp4, vp5, vp4, 136
    vpop vp5, vp0, vp4, vp4, 0x80
    vpop vp6, vp0, vp4, vp4, 0x02
    @!vp5 vcjal 0, vs1, .Ldynproc_kernel_skip0
    @vp5 vsll vv2, vv1, vs51
    @vp5 vlxw vv6, vs60, vv2
    @vp5 vsw vv6, va0
.Ldynproc_kernel_skip0:
    vpop vp4, vp5, vp5, vp5, 0xAA
    vpop vp4, vp4, vp6, vp6, 0xEE
    @!vp4 vcjal 0, vs1, .Ldynproc_kernel_skip1
    vfence
    vaddw vv6, vs0, vs0
    vcmplt vp5, vv6, vs57
.Ldynproc_kernel_skip1:
    vpop vp6, vp4, vp5, vp5, 0x80
    vpop vp7, vp4, vp5, vp5, 0x02
    @!vp6 vcjal 0, vs1, .Ldynproc_kernel_skip2
    vaddw vs2, vs63, vs50
    vsll vv2, vv9, vs45
    vsrl vv2, vv2, vs45
    vcmpeq vp4, vv0, vs49
    vaddw vs3, vs57, vs52
    vsll vs3, vs3, vs45
    vsrl vs3, vs3, vs45
    @vp6 vsll vv2, vv2, vs51
    @vp6 vadd vv3, vv0, vs47
    vcmplt vp5, vv3, vv7
    @vp5 vadd vv3, vv3, vs0
    @!vp5 vadd vv3, vv7, vs0
    @vp6 vsll vv3, vv3, vs51
    vmul vs4, vs48, vs53
    vsll vs4, vs4, vs51
    vsll vs5, vs53, vs51
    @vp6 vsll vv4, vv1, vs51
.Ldynproc_kernel_skip2:
    vpop vp5, vp6, vp6, vp6, 0xAA
    @vp6 vadd vs6, vs0, vs0
    @vp6 vadd vs7, vs0, vs0
    vpclear vp8
.Ldynproc_kernel_loop3:
    vmul vs8, vs5, vs6
    vadd vs8, vs4, vs8
    vadd vs8, vs46, vs8
    vadd vs9, vs6, vs47
    vaddw vs10, vs7, vs56
    vcmpltu vp6, vs6, vv0
    vpop vp9, vp5, vp6, vp6, 0x80
    vpop vp10, vp5, vp6, vp6, 0x02
    @!vp9 vcjal 0, vs1, .Ldynproc_kernel_skip4
    vsubw vs11, vs2, vs7
    vcmplt vp6, vs11, vv0
    vpop vp6, vp6, vp6, vp6, 0x55
    vpop vp11, vp6, vp2, vp1, 0xD8
    vpop vp6, vp11, vp3, vp1, 0xD8
.Ldynproc_kernel_skip4:
    vpop vp11, vp9, vp6, vp6, 0x80
    vpop vp12, vp9, vp6, vp6, 0x02
    @!vp11 vcjal 0, vs1, .Ldynproc_kernel_skip5
    @vp11 vlxw vv6, vs59, vv2
    @vp11 vlw vv7, va0
    @vp11 vlxw vv8, vs59, vv3
    vcmplt vp6, vv6, vv7
    @vp6 vaddw vv7, vv6, vs0
    @!vp6 vaddw vv7, vv7, vs0
    vcmplt vp6, vv7, vv8
    @vp6 vaddw vv7, vv7, vs0
    @!vp6 vaddw vv7, vv8, vs0
    @vp11 vlxw vv6, vs8, vv4
    @vp11 vaddw vv7, vv7, vv6
    @vp11 vsw vv7, va1
    vadd vv5, vs6, vs0
    vcmpeq vp6, vv5, vs0
    vpop vp6, vp4, vp6, vp4, 136
.Ldynproc_kernel_skip5:
    vpop vp9, vp11, vp6, vp6, 0x80
    vpop vp13, vp11, vp6, vp6, 0x02
    @!vp9 vcjal 0, vs1, .Ldynproc_kernel_skip6
    @vp9 vlxw vv6, vs60, vv4
    @vp9 vsll vv5, vv6, vs51
    vaddw vv6, vs56, vs0
    @vp9 vsxw vv6, vs61, vv5
.Ldynproc_kernel_skip6:
    vpop vp6, vp9, vp9, vp9, 0xAA
    vpop vp6, vp6, vp13, vp13, 0xEE
    vpop vp6, vp6, vp12, vp12, 0xEE
    vpop vp6, vp6, vp10, vp10, 0xEE
    vpop vp11, vp10, vp1, vp11, 0xD8
    vpop vp11, vp12, vp1, vp11, 0xD8
    vpop vp11, vp9, vp0, vp11, 0xD8
    vpop vp11, vp13, vp0, vp11, 0xD8
    @!vp6 vcjal 0, vs1, .Ldynproc_kernel_skip7
    vfence
    vadd vv5, vs6, vs0
    vcmpeq vp9, vv5, vs3
.Ldynproc_kernel_skip7:
    vpop vp10, vp6, vp9, vp9, 0x80
    vpop vp12, vp6, vp9, vp9, 0x02
    vpop vp13, vp10, vp11, vp13, 0xD8
    vpop vp8, vp8, vp10, vp10, 0xEE
    vpop vp5, vp5, vp10, vp10, 0x02
    vpop vp6, vp12, vp11, vp11, 0x80
    vpop vp9, vp12, vp11, vp11, 0x02
    @!vp6 vcjal 0, vs1, .Ldynproc_kernel_skip8
    @vp6 vlw vv6, va1
    @vp6 vsw vv6, va0
.Ldynproc_kernel_skip8:
    vpop vp11, vp6, vp6, vp6, 0xAA
    vpop vp11, vp11, vp9, vp9, 0xEE
    vfence
    @vp11 vadd vs6, vs9, vs0
    @vp11 vadd vs7, vs10, vs0
    @vp11 vcjal 1, vs1, .Ldynproc_kernel_loop3
    vpop vp2, vp10, vp10, vp10, 0xAA
    vpop vp2, vp2, vp8, vp8, 0xEE
    vpop vp3, vp2, vp13, vp13, 0x80
    vpop vp4, vp2, vp13, vp13, 0x02
    vpop vp2, vp3, vp3, vp3, 0xAA
    vpop vp2, vp2, vp7, vp7, 0xEE
    @!vp2 vcjal 0, vs1, .Ldynproc_kernel_skip9
    @vp2 vlw vv6, va1
    @vp2 vsll vv1, vv1, vs51
    @vp2 vsxw vv6, vs58, vv1
.Ldynproc_kernel_skip9:
    vpop vp3, vp2, vp2, vp2, 0xAA
    vpop vp3, vp3, vp4, vp4, 0xEE
    vstop

# ---- control threads (from llc) ----
	.file	"hwacha-ct"
