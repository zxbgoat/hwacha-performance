# @kernel micro_inplace
# @n 4096
# @cfg v64=1 vp=1
# @array x elem=8 n=65536
# @va va0 = x
# 同一数组的 load 后 store（原地）：检验混合访存开销是否与地址相关
micro_inplace_vf:
    vpset     vp0
    vld       vv0, va0
    vsd       vv0, va0
    vstop
