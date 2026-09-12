# @kernel sfilter
# @n 65536
# @cfg v64=0 v32=6 vp=0
# @array src elem=4 n=196608
# @array dst elem=4 n=65536
# @va va0 = src
# @va va1 = src offset=1
# @va va2 = src offset=2
# @va va3 = dst
# @vs 3
# 3 点一维模板（高斯模糊的简化）：dst[i] = m0*src[i-1] + m1*src[i] + m2*src[i+1]
sfilter_vf:
    vlw       vv0, va0
    vlw       vv1, va1
    vlw       vv2, va2
    vfmul.s   vv3, vv0, vs1
    vfmadd.s  vv3, vv1, vs2, vv3
    vfmadd.s  vv3, vv2, vs3, vv3
    vsw       vv3, va3
    vstop
