# @kernel micro_branch
# @n 1024
# @cfg v64=1 vp=2
# @vs 1
# 4 个不被采纳的一致性分支（vp1 全假 → any 不成立；语法为 esp 汇编器的 `vcjal <cond>, sd, label`，cond 1 = any）：测量谓词归约与分支解析的代价
micro_branch_vf:
    vpset     vp0
    vpclear   vp1
    @vp1 vcjal 1, vs2, micro_branch_t1
micro_branch_t1:
    @vp1 vcjal 1, vs2, micro_branch_t2
micro_branch_t2:
    @vp1 vcjal 1, vs2, micro_branch_t3
micro_branch_t3:
    @vp1 vcjal 1, vs2, micro_branch_t4
micro_branch_t4:
    vstop
