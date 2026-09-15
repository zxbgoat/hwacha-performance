# @kernel micro_fma2_indep
# @n 4096
# @cfg v32=3 vp=1
# @vs 1
# 两条独立 FMA 写不同寄存器：双 FMA 簇能否并行
micro_fma2_indep_vf:
    vpset     vp0
    vfadd.s vv1, vv0, vs1
    vfsub.s vv2, vv0, vs1
    vstop
