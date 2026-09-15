# @kernel micro_pcmp
# @n 4096
# @cfg v32=1 vp=2
# @vs 1
# vcmp 写谓词后无使用者：谓词比较单元自身的吞吐
micro_pcmp_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    vstop
