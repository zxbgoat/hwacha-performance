// 探针 7：32 位与 64 位单位步长 load/store 在多 lane + 多 bank L2 下的对照（配合 +hwacha_tl_trace 看两条 lane 的请求交错）
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define N 4096
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_load_vf[], micro_store_vf[], micro_load32_vf[], micro_store32_vf[];
static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static double xd[N + 16] __attribute__((aligned(4096)));
static float  xf[N + 16] __attribute__((aligned(4096)));
static void run1(char *blk, long n, unsigned cfg, void *p, int esz) {
  VSETCFG(cfg);
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, p); VF(blk); p = (char *)p + vl * esz; n -= vl; }
  FENCE();
}
#define TIME3(tag, call) do { unsigned long c0, c1; c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm2 %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); } while (0)
int main(void) {
  for (int i = 0; i < N; i++) { xd[i] = i; xf[i] = i; }
  TIME3("load64",  run1(micro_load_vf,    N, VCFG(1, 0, 0, 1), xd, 8));
  TIME3("load32",  run1(micro_load32_vf,  N, VCFG(0, 1, 0, 1), xf, 4));
  TIME3("store64", run1(micro_store_vf,   N, VCFG(1, 0, 0, 1), xd, 8));
  TIME3("store32", run1(micro_store32_vf, N, VCFG(0, 1, 0, 1), xf, 4));
  printf("DONE\n"); return 0;
}
