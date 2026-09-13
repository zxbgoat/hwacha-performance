// 微基准：隔离测量 load / store / copy / 双 load / ALU 链 / FMA 依赖链 / 空块 的 RTL 周期数
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#ifndef N
#define N 4096
#endif
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_load_vf[], micro_store_vf[], micro_copy_vf[], micro_load2_vf[], micro_alu_vf[], micro_fma_dep_vf[], micro_empty_vf[], micro_ldst_vf[], micro_stld_vf[], micro_inplace_vf[];
static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static double xd[N + 16], yd[N + 16];
#define TIME(tag, call) do { unsigned long c0 = cycles_now(); call; unsigned long c1 = cycles_now(); \
  printf("RESULT %s %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); \
  printf("RESULT %s_warm %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); } while (0)
static void run1(char *blk, long n, unsigned cfg, double *p0, double *p1) {
  VSETCFG(cfg); VMCS(vs1, 0x3ff0000000000000ull);
  while (n > 0) { long vl; VSETVL(vl, n); if (p0) VMCA(va0, p0); if (p1) VMCA(va1, p1); VF(blk); if (p0) p0 += vl; if (p1) p1 += vl; n -= vl; }
  FENCE();
}
int main(void) {
  for (int i = 0; i < N; i++) { xd[i] = i; yd[i] = -i; }
  printf("N=%d\n", N);
  TIME("micro_empty",   run1(micro_empty_vf,   N, VCFG(1, 0, 0, 1), 0, 0));
  TIME("micro_load",    run1(micro_load_vf,    N, VCFG(1, 0, 0, 1), xd, 0));
  TIME("micro_store",   run1(micro_store_vf,   N, VCFG(1, 0, 0, 1), yd, 0));
  TIME("micro_copy",    run1(micro_copy_vf,    N, VCFG(1, 0, 0, 1), xd, yd));
  TIME("micro_load2",   run1(micro_load2_vf,   N, VCFG(2, 0, 0, 1), xd, yd));
  TIME("micro_ldst",    run1(micro_ldst_vf,    N, VCFG(2, 0, 0, 1), xd, yd));
  TIME("micro_stld",    run1(micro_stld_vf,    N, VCFG(2, 0, 0, 1), xd, yd));
  TIME("micro_inplace", run1(micro_inplace_vf, N, VCFG(1, 0, 0, 1), xd, 0));
  TIME("micro_alu",     run1(micro_alu_vf,     N, VCFG(2, 0, 0, 1), 0, 0));
  TIME("micro_fma_dep", run1(micro_fma_dep_vf, N, VCFG(2, 0, 0, 1), 0, 0));
  printf("DONE\n");
  return 0;
}
