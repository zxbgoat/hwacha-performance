# @kernel fma_peak
# @n 65536
# @cfg v64=6 vp=1
# @iters 32
# 寄存器内的 FMA 链，无访存：用于验证峰值功能单元吞吐（两个 FMA 簇）
fma_peak_vf:
    vpset     vp0                # 至少需要一个谓词寄存器；未加谓词的指令隐式引用 vp0
    vfmadd.d vv0, vv1, vs1, vv0
    vfmadd.d vv2, vv3, vs2, vv2
    vfmadd.d vv4, vv5, vs3, vv4
    vfmadd.d vv0, vv1, vs1, vv0
    vfmadd.d vv2, vv3, vs2, vv2
    vfmadd.d vv4, vv5, vs3, vv4
    vfmadd.d vv0, vv1, vs1, vv0
    vfmadd.d vv2, vv3, vs2, vv2
    vfmadd.d vv4, vv5, vs3, vv4
    vfmadd.d vv0, vv1, vs1, vv0
    vfmadd.d vv2, vv3, vs2, vv2
    vfmadd.d vv4, vv5, vs3, vv4
    vstop
