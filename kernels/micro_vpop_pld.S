# @kernel micro_vpop_pld
# @n 4096
# @cfg v32=2 vp=3
# @array a elem=4 n=65536
# @va va0 = a
# @vs 2
# vpop 后谓词化 load（pathfinder/pgain 的模式）
micro_vpop_pld_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    @vp1 vlw vv1, va0
    vstop
