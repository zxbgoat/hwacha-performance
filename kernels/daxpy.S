# @kernel daxpy
# @n 65536
# @cfg v64=2 vp=0
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
daxpy_vf:
    vld       vv0, va0
    vld       vv1, va1
    vfmadd.d  vv1, vv0, vs1, vv1
    vsd       vv1, va1
    vstop
