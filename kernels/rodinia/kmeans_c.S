# @kernel kmeans_c
# @n 1024
# @cfg v64=8 vp=6
# @array a0 elem=4 n=1024
# @va va0 = a0
# @vs 14
# 由 scripts/extract_hcc_kernel.py 从 kmeans.s 的 kmeans_kernel_c_wt 区域抠出（多入口块按原地址顺序连续排列，
# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。
kmeans_kernel_c_wt:
    vpset vp0
    veidx vv0
    vadd vv0, vv0, vs63
    vcmpltu vp1, vv0, vs61
    vpop vp2, vp0, vp1, vp1, 0x80
    vpop vp3, vp0, vp1, vp1, 0x02
    @!vp2 vcjal 0, vs1, .Lkmeans_kernel_c_skip0
    vaddw vv3, vs0, vs0
    vcmplt vp1, vv3, vs60
.Lkmeans_kernel_c_skip0:
    vpop vp4, vp2, vp1, vp1, 0x80
    vpop vp5, vp2, vp1, vp1, 0x02
    @!vp4 vcjal 0, vs1, .Lkmeans_kernel_c_skip1
    vaddw vv3, vs0, vs0
    vcmplt vp1, vv3, vs59
    vsll vs2, vs59, vs54
    vsrl vs2, vs2, vs54
    vsll vs3, vs60, vs54
    vsrl vs3, vs3, vs54
    vsll vs4, vs61, vs54
    vsrl vs4, vs4, vs54
.Lkmeans_kernel_c_skip1:
    @vp4 vaddw vv3, vs58, vs0
    @vp4 vaddw vv4, vs0, vs0
    vstop
kmeans_kernel_c_wt_r0_b0:
    vaddw vv6, vs0, vs0
    vstop
kmeans_kernel_c_wt_r0_b1:
    vaddw vv5, vs0, vs0
    vstop
kmeans_kernel_c_wt_r0_b2:
    @vp4 vadd vv1, vv0, vs5
    vsll vv2, vv1, vs54
    vsrl vv2, vv2, vs54
    @vp4 vsll vv2, vv2, vs55
    @vp4 vlxw vv7, vs62, vv2
    @vp4 vfsub.s vv7, vv7, vs6
    @vp4 vfmadd.s vv5, vv7, vv7, vv5
    vaddw vv7, vv5, vs0
    vstop
kmeans_kernel_c_wt_r0_b3:
    vaddw vv6, vv7, vs0
    vstop
kmeans_kernel_c_wt_r0_b4:
    vcmpflt.s vp2, vv6, vv3
    @vp2 vaddw vv4, vs7, vs0
    @vp2 vaddw vv3, vv6, vs0
    vstop
kmeans_kernel_c_wt_a0:
    vpop vp1, vp4, vp4, vp4, 0xAA
    vpop vp1, vp1, vp5, vp5, 0xEE
    @vp5 vaddw vv3, vs0, vs0
    @vp4 vaddw vv3, vv4, vs0
    @vp1 vsw vv3, va0
    vpop vp2, vp1, vp1, vp1, 0xAA
    vpop vp2, vp2, vp3, vp3, 0xEE
    vstop
