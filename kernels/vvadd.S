# @kernel vvadd
# @n 65536
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# @ctrl 6
# y[i] = x[i] + y[i]，纯流式，访存受限
vvadd_vf:
    vpset     vp0                # 至少需要一个谓词寄存器；未加谓词的指令隐式引用 vp0
    vld     vv0, va0
    vld     vv1, va1
    vfadd.d vv0, vv0, vv1
    vsd     vv0, va1
    vstop
