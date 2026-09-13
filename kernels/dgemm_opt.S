# @kernel dgemm_opt
# @n 65536
# @cfg v64=10 vp=1
# @array B elem=8 n=16384
# @va va1 = B
# @va va3 = B offset=8192
# @vs 8
# @keep
# 论文 gemm-opt 风格的寄存器分块内积：8 个 C 向量常驻 VRF（vv0–vv7），
# 每次 vf 流入两行 B（单位步长），A 的 8 个标量由 vmcs 送入 vs1–vs8。
# C 的加载/存储每 K 步只做一次，这里省略（用 @keep 说明）。B 块 128 KB 驻留 L2，
# va 越过数组末尾后回绕，表示同一 B 块被后续 C 分块复用。
dgemm_vf:
    vpset     vp0                # 至少需要一个谓词寄存器；未加谓词的指令隐式引用 vp0
    vld       vv8, va1
    vld       vv9, va3
    vfmadd.d  vv0, vv8, vs1, vv0
    vfmadd.d  vv1, vv8, vs2, vv1
    vfmadd.d  vv2, vv8, vs3, vv2
    vfmadd.d  vv3, vv8, vs4, vv3
    vfmadd.d  vv4, vv9, vs5, vv4
    vfmadd.d  vv5, vv9, vs6, vv5
    vfmadd.d  vv6, vv9, vs7, vv6
    vfmadd.d  vv7, vv9, vs8, vv7
    vstop
