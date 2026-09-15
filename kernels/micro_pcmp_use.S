# @kernel micro_pcmp_use
# @n 4096
# @cfg v32=2 vp=2
# @vs 1
# vcmp 写谓词，紧接着谓词化算术读它：谓词 RAW 的延迟
micro_pcmp_use_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    @vp1 vfadd.s vv1, vv0, vs1
    vstop
