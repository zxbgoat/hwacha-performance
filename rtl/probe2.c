// 探测：vsetcfg/vmcs 开销、完整 stripmine 循环、以及"标量核刚写过的数据（脏在 L1D）"对首次向量访问的影响
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_empty_vf[], micro_load_vf[], micro_store_vf[], micro_copy_vf[];
static inline unsigned long cyc(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
#define N 4096
static double xd[N + 16], yd[N + 16];
static void run1(char *blk, long n, unsigned cfg, double *p0, double *p1) {
  VSETCFG(cfg); VMCS(vs1, 0x3ff0000000000000ull);
  while (n > 0) { long vl; VSETVL(vl, n); if (p0) VMCA(va0, p0); if (p1) VMCA(va1, p1); VF(blk); if (p0) p0 += vl; if (p1) p1 += vl; n -= vl; }
  FENCE();
}
#define T(tag, call) do { unsigned long c0 = cyc(); call; unsigned long c1 = cyc(); printf("PROBE %s %lu\n", tag, c1 - c0); } while (0)
int main(void) {
  for (int i = 0; i < N; i++) { xd[i] = i; yd[i] = -i; }
  T("vsetcfg", VSETCFG(VCFG(1, 0, 0, 1)));
  T("vsetcfg", VSETCFG(VCFG(1, 0, 0, 1)));
  T("vmcs", VMCS(vs1, 1));
  T("vmcs", VMCS(vs1, 1));
  T("fence_idle", FENCE());
  T("empty_loop_1st", run1(micro_empty_vf, N, VCFG(1, 0, 0, 1), 0, 0));
  T("empty_loop_2nd", run1(micro_empty_vf, N, VCFG(1, 0, 0, 1), 0, 0));
  T("load_1st", run1(micro_load_vf, N, VCFG(1, 0, 0, 1), xd, 0));      // xd 刚由标量核写过
  T("load_2nd", run1(micro_load_vf, N, VCFG(1, 0, 0, 1), xd, 0));      // 已被向量单元读过一遍
  T("load_3rd", run1(micro_load_vf, N, VCFG(1, 0, 0, 1), xd, 0));
  T("store_1st", run1(micro_store_vf, N, VCFG(1, 0, 0, 1), yd, 0));    // yd 刚由标量核写过
  T("store_2nd", run1(micro_store_vf, N, VCFG(1, 0, 0, 1), yd, 0));
  T("store_3rd", run1(micro_store_vf, N, VCFG(1, 0, 0, 1), yd, 0));
  T("copy_1st", run1(micro_copy_vf, N, VCFG(1, 0, 0, 1), xd, yd));
  T("copy_2nd", run1(micro_copy_vf, N, VCFG(1, 0, 0, 1), xd, yd));
  for (int i = 0; i < N; i++) { xd[i] = i + 1; }                        // 标量核再次写 xd → 部分脏在 L1D
  T("load_after_scalar_write", run1(micro_load_vf, N, VCFG(1, 0, 0, 1), xd, 0));
  T("load_again", run1(micro_load_vf, N, VCFG(1, 0, 0, 1), xd, 0));
  printf("DONE\n");
  return 0;
}
