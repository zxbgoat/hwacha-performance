# @kernel micro_store
# @n 4096
# @cfg v64=1 vp=1
# @array y elem=8 n=65536
# @va va0 = y
# 只有 store：测量向量 store 通路的吞吐（vv0 内容任意）
micro_store_vf:
    vpset     vp0
    vsd       vv0, va0
    vstop
