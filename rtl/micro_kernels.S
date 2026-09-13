.text
.globl micro_load_vf, micro_store_vf, micro_copy_vf, micro_load2_vf, micro_alu_vf, micro_fma_dep_vf, micro_empty_vf, micro_ldst_vf, micro_stld_vf, micro_inplace_vf
.align 3
#include "../kernels/micro_load.S"
.align 3
#include "../kernels/micro_store.S"
.align 3
#include "../kernels/micro_copy.S"
.align 3
#include "../kernels/micro_load2.S"
.align 3
#include "../kernels/micro_alu.S"
.align 3
#include "../kernels/micro_fma_dep.S"
.align 3
#include "../kernels/micro_empty.S"
.align 3
#include "../kernels/micro_ldst.S"
.align 3
#include "../kernels/micro_stld.S"
.align 3
#include "../kernels/micro_inplace.S"
