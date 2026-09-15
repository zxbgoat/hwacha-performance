# @kernel micro_fma2_waw
# @n 4096
# @cfg v32=2 vp=1
# @vs 1
# 两条独立 FMA 写同一寄存器（WAW）：写口/冒险检查的代价
micro_fma2_waw_vf:
    vpset     vp0
    vfadd.s vv1, vv0, vs1
    vfsub.s vv1, vv0, vs1
    vstop
