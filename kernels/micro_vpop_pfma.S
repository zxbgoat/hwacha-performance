# @kernel micro_vpop_pfma
# @n 4096
# @cfg v32=2 vp=3
# @vs 2
# vpop 后两条互斥谓词化 FMA（对照 pcmp_2use）
micro_vpop_pfma_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    @vp1 vfadd.s vv1, vv0, vs1
    @!vp1 vfsub.s vv1, vv0, vs1
    vstop
