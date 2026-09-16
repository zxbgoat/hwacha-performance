# @kernel micro_pcmp_fp_int
# @n 4096
# @cfg v32=2 vp=2
# @vs 2
# vcmp 后浮点 + 整数各一条互斥谓词化（clamp 原样，无访存）
micro_pcmp_fp_int_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vfmul.s vv1, vs1, vv0
    @!vp1 vaddw vv1, vs0, vs0
    vstop
