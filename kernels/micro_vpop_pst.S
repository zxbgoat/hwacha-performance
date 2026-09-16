# @kernel micro_vpop_pst
# @n 4096
# @cfg v32=2 vp=3
# @array a elem=4 n=65536
# @va va0 = a
# @vs 2
# vpop 后谓词化 store
micro_vpop_pst_vf:
    vpset     vp0
    vpop vp1, vp0, vp0, vp0, 0xAA
    @vp1 vsw vv0, va0
    vstop
