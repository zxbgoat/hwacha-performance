// 探测 vf 块内标量指令与一致性分支的代价（vl = 8 与 1024）
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_sload_vf[], micro_smul_vf[], micro_salu_vf[], micro_branch_vf[], micro_empty_vf[];
static inline unsigned long cyc(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static uint64_t chain[64];
#define T(tag, vlreq, blk) do { long vl; VSETVL(vl, vlreq); unsigned long c0 = cyc(); VF(blk); FENCE(); unsigned long c1 = cyc(); \
  printf("PROBE %s vl=%ld %lu\n", tag, vl, c1 - c0); } while (0)
int main(void) {
  for (int i = 0; i < 64; i++) chain[i] = (uint64_t)&chain[(i + 1) % 64];   // 指针追逐链
  VSETCFG(VCFG(1, 0, 0, 2)); VMCS(vs1, (unsigned long)chain);
  for (int rep = 0; rep < 2; rep++) {
    T("empty", 8, micro_empty_vf);   T("empty", 1024, micro_empty_vf);
    T("sload4", 8, micro_sload_vf);  T("sload4", 1024, micro_sload_vf);
    VMCS(vs1, 3);
    T("smul4", 8, micro_smul_vf);    T("smul4", 1024, micro_smul_vf);
    T("salu8", 8, micro_salu_vf);    T("salu8", 1024, micro_salu_vf);
    T("branch4", 8, micro_branch_vf); T("branch4", 1024, micro_branch_vf);
    VMCS(vs1, (unsigned long)chain);
  }
  printf("DONE\n");
  return 0;
}
