# @kernel micro_copy
# @n 4096
# @cfg v64=1 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# load + store：y[i] = x[i]
micro_copy_vf:
    vpset     vp0
    vld       vv0, va0
    vsd       vv0, va1
    vstop
