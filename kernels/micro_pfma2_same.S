# @kernel micro_pfma2_same
# @n 4096
# @cfg v32=2 vp=2
# @vs 1
# 两条同谓词的 FMA 写同一寄存器（无 vcmp）
micro_pfma2_same_vf:
    vpset     vp0
    @vp0 vfadd.s vv1, vv0, vs1
    @vp0 vfsub.s vv1, vv0, vs1
    vstop
