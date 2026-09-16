# @kernel micro_pcmp_br
# @n 4096
# @cfg v32=2 vp=3
# @vs 2
# vcmp 后 vpop 再一致性分支（divloop 的模式）
micro_pcmp_br_vf:
    vpset     vp0
    vcmpflt.s vp1, vs1, vv0
    vpop vp2, vp0, vp1, vp1, 0x80
    @!vp2 vcjal 0, vs2, .Lpb_skip
    @vp1 vfadd.s vv1, vv0, vs1
.Lpb_skip:
    vstop
