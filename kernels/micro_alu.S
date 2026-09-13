# @kernel micro_alu
# @n 4096
# @cfg v64=2 vp=1
# @iters 13
# 寄存器内整数加法链：bank 本地 ALU，读口 2 拍/strip
micro_alu_vf:
    vpset     vp0
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vadd      vv0, vv0, vv1
    vstop
