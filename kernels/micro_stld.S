# @kernel micro_stld
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# store 在前、load 在后（无依赖）：检验混合访存开销是否与顺序有关
micro_stld_vf:
    vpset     vp0
    vsd       vv1, va1
    vld       vv0, va0
    vstop
