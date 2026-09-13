# @kernel micro_salu
# @n 1024
# @cfg v64=1 vp=1
# @vs 1
# 块内 8 条相关的标量加法：测量标量单元本地流水
micro_salu_vf:
    vpset     vp0
    vadd      vs2, vs1, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vadd      vs2, vs2, vs1
    vstop
