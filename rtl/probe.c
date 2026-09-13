// 探测 vf 的固定开销：空块的发射时间与 fence 等待时间，vl=8 与 vl=2048 两种
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_empty_vf[], micro_load_vf[];
static inline unsigned long cyc(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
static double xd[4096];
int main(void) {
  asm volatile("vsetcfg %0" :: "r"((unsigned long)VCFG(1, 0, 0, 1)));
  for (int i = 0; i < 4096; i++) xd[i] = i;
  long vls[2] = {8, 2048};
  for (int v = 0; v < 2; v++) {
    for (int rep = 0; rep < 3; rep++) {
      unsigned long t0 = cyc();
      long vl; asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(vls[v]));
      unsigned long t1 = cyc();
      asm volatile("vf 0(%0)" :: "r"(micro_empty_vf));
      unsigned long t2 = cyc();
      asm volatile("fence" ::: "memory");
      unsigned long t3 = cyc();
      printf("PROBE empty vl=%ld vsetvl=%lu vf=%lu fence=%lu\n", vl, t1 - t0, t2 - t1, t3 - t2);
    }
    for (int rep = 0; rep < 2; rep++) {
      unsigned long t0 = cyc();
      long vl; asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(vls[v]));
      asm volatile("vmca va0, %0" :: "r"(xd));
      unsigned long t1 = cyc();
      asm volatile("vf 0(%0)" :: "r"(micro_load_vf));
      unsigned long t2 = cyc();
      asm volatile("fence" ::: "memory");
      unsigned long t3 = cyc();
      printf("PROBE load vl=%ld setup=%lu vf=%lu fence=%lu\n", vl, t1 - t0, t2 - t1, t3 - t2);
    }
  }
  // 连续两个 vf（不 fence）：第二个 vf 的发射是否被阻塞
  { long vl; asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(2048L)); asm volatile("vmca va0, %0" :: "r"(xd));
    unsigned long t0 = cyc(); asm volatile("vf 0(%0)" :: "r"(micro_load_vf)); unsigned long t1 = cyc();
    asm volatile("vf 0(%0)" :: "r"(micro_load_vf)); unsigned long t2 = cyc();
    asm volatile("vf 0(%0)" :: "r"(micro_load_vf)); unsigned long t3 = cyc();
    asm volatile("fence" ::: "memory"); unsigned long t4 = cyc();
    printf("PROBE 3xload vl=%ld vf1=%lu vf2=%lu vf3=%lu fence=%lu\n", vl, t1 - t0, t2 - t1, t3 - t2, t4 - t3); }
  printf("DONE\n");
  return 0;
}
