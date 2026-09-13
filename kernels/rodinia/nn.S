# @kernel nn
# @n 2048
# @cfg v64=4 vp=4
# @array a0 elem=4 n=2048
# @va va0 = a0
# @array a1 elem=4 n=2048
# @va va1 = a1
# @array a2 elem=4 n=2048
# @va va2 = a2
# @array a3 elem=4 n=2048
# @va va3 = a3
# @array a4 elem=4 n=2048
# @va va4 = a4
# @vs 4
# 由 scripts/extract_hcc_kernel.py 从 nn.s 的 NearestNeighbor_wt 区域抠出（多入口块按原地址顺序连续排列，
# 踪迹模式下用 --trace-base <首指令地址> 把踪迹 pc 映射到这里的指令序号）。
NearestNeighbor_wt:
    vpset vp0
    veidx vv0
    vadd vv0, vv0, vs63
    vcmplt vp1, vv0, vs62
    vpop vp2, vp0, vp1, vp1, 0x80
    vpop vp3, vp0, vp1, vp1, 0x02
    @!vp2 vcjal 0, vs1, .LNearestNeighbor_skip0
    @vp2 vlstw vv1, va0, va3
    @vp2 vfsub.s vv2, vs61, vv1
    @vp2 vlstw vv1, va1, va4
    @vp2 vfsub.s vv3, vs60, vv1
    @vp2 vfmul.s vv3, vv3, vv3
    @vp2 vfmadd.s vv2, vv2, vv2, vv3
    @vp2 vfsqrt.s vv2, vv2
    @vp2 vsw vv2, va2
.LNearestNeighbor_skip0:
    vpop vp1, vp2, vp2, vp2, 0xAA
    vpop vp1, vp1, vp3, vp3, 0xEE
    vstop

# ---- control threads (from llc) ----
	.file	"hwacha-ct"
