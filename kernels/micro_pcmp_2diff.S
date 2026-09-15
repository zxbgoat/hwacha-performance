# @kernel micro_pcmp_2diff
# @n 4096
# @cfg v32=3 vp=2
# @vs 1
# vcmp 后两条互斥谓词化 FMA 写不同寄存器：把谓词与 WAW 分开
micro_pcmp_2diff_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vfadd.s vv1, vv0, vs1
    @!vp1 vfsub.s vv2, vv0, vs1
    vstop
