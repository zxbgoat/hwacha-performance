# @kernel kmeans_swap
# @n 1024
# @cfg v64=7 vp=4
# @vs 14
# 由 scripts/extract_hcc_kernel.py 从 kmeans.s 的 kmeans_swap_wt 区域抠出（多入口块按原地址顺序连续排列，
# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。
kmeans_swap_wt:
    vpset vp0
    veidx vv0
    vadd vv0, vv0, vs63
    vcmpltu vp1, vv0, vs60
    vaddw vv5, vs0, vs0
    vcmplt vp2, vv5, vs59
    vpop vp1, vp1, vp2, vp1, 136
    vpop vp2, vp0, vp1, vp1, 0x80
    vpop vp3, vp0, vp1, vp1, 0x02
    @!vp2 vcjal 0, vs1, .Lkmeans_swap_skip0
    @vp2 vmulw vv5, vs59, vv0
    vsll vs2, vs59, vs56
    vsrl vs2, vs2, vs56
    vsll vv1, vv5, vs56
    vsrl vv1, vv1, vs56
    vsll vs3, vs60, vs56
    vsrl vs3, vs3, vs56
.Lkmeans_swap_skip0:
    vstop
kmeans_swap_wt_r0_b0:
    @vp2 vadd vv2, vv1, vs4
    vsll vv3, vv2, vs56
    vsrl vv3, vv3, vs56
    @vp2 vsll vv3, vv3, vs58
    @vp2 vadd vv2, vv0, vs5
    vsll vv4, vv2, vs56
    vsrl vv4, vv4, vs56
    @vp2 vsll vv4, vv4, vs58
    @vp2 vlxw vv6, vs61, vv3
    @vp2 vsxw vv6, vs62, vv4
    vstop
kmeans_swap_wt_a0:
    vpop vp1, vp2, vp2, vp2, 0xAA
    vpop vp1, vp1, vp3, vp3, 0xEE
    vstop

# ---- control threads (from llc) ----
	.file	"hwacha-ct"
