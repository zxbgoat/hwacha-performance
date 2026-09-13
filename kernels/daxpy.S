# @kernel daxpy
# @n 65536
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
daxpy_vf:
    vpset     vp0                # 至少需要一个谓词寄存器；未加谓词的指令隐式引用 vp0
    vld       vv0, va0
    vld       vv1, va1
    vfmadd.d  vv1, vv0, vs1, vv1
    vsd       vv1, va1
    vstop
