# @kernel vvadd
# @n 65536
# @cfg v64=2 vp=0
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# @ctrl 6
# y[i] = x[i] + y[i]，纯流式，访存受限
vvadd_vf:
    vld     vv0, va0
    vld     vv1, va1
    vfadd.d vv0, vv0, vv1
    vsd     vv0, va1
    vstop
