# @kernel micro_pcmp_int2
# @n 4096
# @cfg v32=2 vp=2
# @vs 2
# vcmp 后两条互斥的整数类谓词化操作
micro_pcmp_int2_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vaddw vv1, vv0, vs1
    @!vp1 vsubw vv1, vv0, vs1
    vstop
