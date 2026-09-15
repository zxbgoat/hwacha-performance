# @kernel micro_st2
# @n 4096
# @cfg v64=2 vp=1
# @array x elem=8 n=65536
# @array y elem=8 n=65536
# @va va0 = x
# @va va1 = y
# 两条独立的 store 流：每条 lane 交替写两行，检验 L2 store 并发代价模型对"每 lane 多于一条流"的推广
micro_st2_vf:
    vpset     vp0
    vsd       vv0, va0
    vsd       vv1, va1
    vstop
