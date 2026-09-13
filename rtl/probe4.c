// 探测主存路径：先用标量核写 2 MB 把 L2 冲掉，再向量 load / store 一段不在 L2 中的数据
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_load_vf[], micro_store_vf[];
static inline unsigned long cyc(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
#define N 4096
static double xd[N + 16], yd[N + 16];
static double flush[262144];          // 2 MB
static void run1(char *blk, long n, double *p0) {
  VSETCFG(VCFG(1, 0, 0, 1));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, p0); VF(blk); p0 += vl; n -= vl; }
  FENCE();
}
static void evict(void) { for (long i = 0; i < 262144; i += 8) flush[i] = i; }
#define T(tag, call) do { unsigned long c0 = cyc(); call; unsigned long c1 = cyc(); printf("PROBE %s %lu\n", tag, c1 - c0); } while (0)
int main(void) {
  for (int i = 0; i < N; i++) { xd[i] = i; yd[i] = -i; }
  T("load_warm", run1(micro_load_vf, N, xd)); T("load_warm", run1(micro_load_vf, N, xd));
  evict();
  T("load_from_dram", run1(micro_load_vf, N, xd));      // 32 KB，全部 L2 缺失
  T("load_warm", run1(micro_load_vf, N, xd));
  evict();
  T("load_from_dram_1strip", run1(micro_load_vf, 8, xd + 2048));   // 单个 strip：主存延迟
  T("load_from_dram_1strip", run1(micro_load_vf, 8, xd + 2560));
  T("load_from_dram_1strip", run1(micro_load_vf, 8, xd + 3072));
  evict();
  T("store_to_dram", run1(micro_store_vf, N, yd));
  T("store_warm", run1(micro_store_vf, N, yd));
  printf("DONE\n");
  return 0;
}
