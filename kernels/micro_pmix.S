# @kernel micro_pmix
# @n 4096
# @cfg v32=2 vp=3
# @vs 1
# vpop 与谓词化算术交错（pgain/pathfinder 的模式）
micro_pmix_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    @vp1 vfadd.s vv1, vv0, vs1
    vpop vp2, vp1, vp1, vp1, 0xEE
    @vp2 vfadd.s vv1, vv1, vs1
    vstop
