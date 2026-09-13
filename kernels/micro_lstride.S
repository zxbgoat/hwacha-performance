# @kernel micro_lstride
# @n 4096
# @cfg v32=1 vp=1
# @array x elem=4 n=65536
# @va va0 = x stride=2
# @va va1 = 8
# 跨步 load：32 位元素、步长 8 B（Rodinia nn 的 LatLong 结构体访问），测量 VMU 非单位步长的每元素代价
micro_lstride_vf:
    vpset     vp0
    vlstw     vv0, va0, va1
    vstop
