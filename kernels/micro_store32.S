# @kernel micro_store32
# @n 4096
# @cfg v32=1 vp=1
# @array y elem=4 n=65536
# @va va0 = y
# 32 位单位步长 store
micro_store32_vf:
    vpset     vp0
    vsw       vv0, va0
    vstop
