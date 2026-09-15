# @kernel micro_pcmp_2use
# @n 4096
# @cfg v32=2 vp=2
# @vs 1
# vcmp 后两条互斥谓词化算术（clamp 的模式）
micro_pcmp_2use_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vfadd.s vv1, vv0, vs1
    @!vp1 vfsub.s vv1, vv0, vs1
    vstop
