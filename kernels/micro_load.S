# @kernel micro_load
# @n 4096
# @cfg v64=1 vp=1
# @array x elem=8 n=65536
# @va va0 = x
# 只有 load：测量向量 load 通路的吞吐
micro_load_vf:
    vpset     vp0
    vld       vv0, va0
    vstop
