# @kernel micro_pcmp_int
# @n 4096
# @cfg v32=2 vp=2
# @vs 2
# vcmp 后一条整数类谓词化操作（clamp 的第二条）
micro_pcmp_int_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vaddw vv1, vv0, vs1
    vstop
