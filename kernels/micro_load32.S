# @kernel micro_load32
# @n 4096
# @cfg v32=1 vp=1
# @array x elem=4 n=65536
# @va va0 = x
# 32 位单位步长 load：多 lane 时每条 lane 一次拿 8 个元素 = 32 B，两条 lane 落在同一行（2 bank L2 的锁步/同行效应探针）
micro_load32_vf:
    vpset     vp0
    vlw       vv0, va0
    vstop
