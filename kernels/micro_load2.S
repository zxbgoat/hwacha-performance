# @kernel micro_load2
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# 两条独立 load：测量两个 load 操作在 VMU 中的重叠程度
micro_load2_vf:
    vpset     vp0
    vld       vv0, va0
    vld       vv1, va1
    vstop
