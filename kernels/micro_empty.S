# @kernel micro_empty
# @n 4096
# @cfg v64=1 vp=1
# 空块：测量每次 vf 的固定开销（控制线程 + 前端 + 退休）
micro_empty_vf:
    vpset     vp0
    vstop
