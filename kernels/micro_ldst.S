# @kernel micro_ldst
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# 无依赖的 load + store（store 的数据寄存器与 load 无关）：区分 copy 的额外开销来自数据依赖还是访存混合
micro_ldst_vf:
    vpset     vp0
    vld       vv0, va0
    vsd       vv1, va1
    vstop
