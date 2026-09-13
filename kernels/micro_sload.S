# @kernel micro_sload
# @n 1024
# @cfg v64=1 vp=1
# @array x elem=8 n=1024
# @va va0 = x
# @vs 1
# 块内 4 条相关的标量 load（SMU 路径）：测量 vf 块内标量访存的延迟
micro_sload_vf:
    vpset     vp0
    vlsd      vs2, vs1
    vlsd      vs3, vs2
    vlsd      vs4, vs3
    vlsd      vs5, vs4
    vstop
