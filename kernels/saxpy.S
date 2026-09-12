# @kernel saxpy
# @n 65536
# @cfg v64=0 v32=2 vp=0
# @array x elem=4 n=65536
# @array y elem=4 n=65536
# @va va0 = x
# @va va1 = y
# y[i] = a*x[i] + y[i]，单精度；a 在 vs1（循环外 vmcs）
saxpy_vf:
    vlw       vv0, va0
    vlw       vv1, va1
    vfmadd.s  vv1, vv0, vs1, vv1
    vsw       vv1, va1
    vstop
