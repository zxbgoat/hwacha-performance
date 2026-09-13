# @kernel micro_fma_dep
# @n 4096
# @cfg v64=2 vp=1
# @iters 13
# 单条 FMA 依赖链（每条都依赖上一条的结果）：测 chaining 与 FMA 延迟
micro_fma_dep_vf:
    vpset     vp0
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vfmadd.d  vv0, vv0, vs1, vv1
    vstop
