# @kernel micro_vpop_indep
# @n 4096
# @cfg v64=1 vp=5
# @vs 1
# 独立的 vpop：谓词逻辑单元吞吐
micro_vpop_indep_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    vpop vp2, vp0, vp0, vp0, 0xEE
    vpop vp3, vp0, vp0, vp0, 0xAA
    vpop vp4, vp0, vp0, vp0, 0xEE
    vstop
