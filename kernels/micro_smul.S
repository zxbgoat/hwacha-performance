# @kernel micro_smul
# @n 1024
# @cfg v64=1 vp=1
# @vs 1
# 块内 4 条相关的标量乘法：测量标量乘除单元延迟
micro_smul_vf:
    vpset     vp0
    vmul      vs2, vs1, vs1
    vmul      vs3, vs2, vs1
    vmul      vs4, vs3, vs1
    vmul      vs5, vs4, vs1
    vstop
