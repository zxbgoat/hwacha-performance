// 把性能模型使用的内核文件原样汇编进来：`# @` 指令对汇编器是注释，`.align 3` 保证 vf 块 8 字节对齐
.text
.globl vvadd_vf, saxpy_vf, daxpy_vf, csaxpy_vf, sfilter_vf, gather_vf, dgemm_vf, fma_peak_vf
.align 3
#include "../kernels/vvadd.S"
.align 3
#include "../kernels/saxpy.S"
.align 3
#include "../kernels/daxpy.S"
.align 3
#include "../kernels/csaxpy.S"
.align 3
#include "../kernels/sfilter.S"
.align 3
#include "../kernels/gather.S"
.align 3
#include "../kernels/dgemm_opt.S"
.align 3
#include "../kernels/fma_peak.S"
