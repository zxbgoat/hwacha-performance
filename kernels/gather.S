# @kernel gather
# @n 16384
# @cfg v64=3 vp=1
# @array idx elem=8 n=16384
# @array table elem=8 n=4096
# @array out elem=8 n=16384
# @va va0 = idx
# @va va1 = out
# 随机 gather：out[i] = table[idx[i]]（表 32 KB，8 页恰好覆盖 8 项 DTLB；把 n 调大即可看到 TLB 缺失主导）。索引访存每元素一个 beat，受 VMT 在途上限、8 项 TLB 与 L2 延迟影响
gather_vf:
    vpset     vp0                # 至少需要一个谓词寄存器；未加谓词的指令隐式引用 vp0
    vld       vv0, va0
    vlxd      vv1, vs1, vv0        # @gather table random
    vsd       vv1, va1
    vstop
