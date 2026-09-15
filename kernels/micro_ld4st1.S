# @kernel micro_ld4st1
# @n 4096
# @cfg v64=4 vp=1
# @array a elem=8 n=65536
# @array b elem=8 n=65536
# @array c elem=8 n=65536
# @array d elem=8 n=65536
# @va va0 = a
# @va va1 = b
# @va va2 = c
# @va va3 = d
# 四个 load 流夹一个 store 流（store 只占 1/5 的 beat）：检验 store 并发代价在以 load 为主的流量里的表现
micro_ld4st1_vf:
    vpset     vp0
    vld       vv0, va0
    vld       vv1, va1
    vld       vv2, va2
    vld       vv3, va3
    vfadd.d   vv0, vv0, vv1
    vfadd.d   vv2, vv2, vv3
    vfadd.d   vv0, vv0, vv2
    vsd       vv0, va0
    vstop
