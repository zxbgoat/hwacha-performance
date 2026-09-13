# @kernel micro_sstride
# @n 4096
# @cfg v32=1 vp=1
# @array y elem=4 n=65536
# @va va0 = y stride=2
# @va va1 = 8
# 跨步 store：32 位元素、步长 8 B
micro_sstride_vf:
    vpset     vp0
    vsstw     vv0, va0, va1
    vstop
