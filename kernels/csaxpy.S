# @kernel csaxpy
# @n 65536
# @cfg v64=0 v32=2 vp=1
# @array cond elem=1 n=65536
# @array x elem=4 n=65536
# @array y elem=4 n=65536
# @va va0 = cond
# @va va1 = x
# @va va2 = y
# 论文图 6.2 的条件 SAXPY：谓词化访存与 FMA
csaxpy_vf:
      vlb       vv0, va0
      vcmpez    vp0, vv0
 !vp0 vlw       vv0, va1
 !vp0 vlw       vv1, va2
 !vp0 vfmadd.s  vv0, vv0, vs1, vv1
 !vp0 vsw       vv0, va2
      vstop
