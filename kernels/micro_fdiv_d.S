# @kernel micro_fdiv_d
# @n 4096
# @cfg v64=2 vp=1
# @vs 1
# 双精度除法：先用 vs1（2.0）把 vv1 填成正常数 4.0（未初始化的 VRF 是 0，hardfloat 对 0/NaN 走特殊值快速路径），再测 FDiv/FSqrt 单元的每元素代价
micro_fdiv_d_vf:
    vpset     vp0
    vfadd.d   vv1, vs1, vs1
    vfdiv.d   vv0, vv1, vs1
    vstop
