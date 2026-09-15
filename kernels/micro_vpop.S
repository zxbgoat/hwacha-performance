# @kernel micro_vpop
# @n 4096
# @cfg v64=1 vp=3
# @vs 1
# 相关的 vpop 链：谓词逻辑单元吞吐与延迟
micro_vpop_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    vpop vp2, vp1, vp1, vp1, 0xEE
    vpop vp1, vp2, vp2, vp2, 0xAA
    vpop vp2, vp1, vp1, vp1, 0xEE
    vstop
