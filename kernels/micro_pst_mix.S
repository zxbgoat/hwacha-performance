# @kernel micro_pst_mix
# @n 4096
# @cfg v32=2 vp=2
# @array a elem=4 n=65536
# @va va0 = a
# @array b elem=4 n=65536
# @va va1 = b
# @vs 2
# 谓词化 store 与无谓词 store 交错
micro_pst_mix_vf:
    vpset     vp0
    @vp0 vsw vv0, va0
    vsw vv1, va1
    vstop
