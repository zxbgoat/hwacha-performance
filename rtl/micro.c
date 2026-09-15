// 微基准：隔离测量 load / store / copy / 双 load / ALU 链 / FMA 依赖链 / 空块 的 RTL 周期数
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#ifndef N
#define N 4096
#endif
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_load_vf[], micro_store_vf[], micro_copy_vf[], micro_load2_vf[], micro_alu_vf[], micro_fma_dep_vf[], micro_empty_vf[], micro_ldst_vf[], micro_stld_vf[], micro_inplace_vf[], micro_fsqrt_s_vf[], micro_fdiv_s_vf[], micro_fdiv_d_vf[], micro_lstride_vf[], micro_sstride_vf[], micro_st2_vf[], micro_ld4st1_vf[];
static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static double xd[N + 16] __attribute__((aligned(4096))), yd[N + 16] __attribute__((aligned(4096)));
#define TIME(tag, call) do { unsigned long c0 = cycles_now(); call; unsigned long c1 = cycles_now(); \
  printf("RESULT %s %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); \
  printf("RESULT %s_warm %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); } while (0)
static void run1(char *blk, long n, unsigned cfg, double *p0, double *p1) {
  VSETCFG(cfg); VMCS(vs1, 0x3ff0000000000000ull);
  while (n > 0) { long vl; VSETVL(vl, n); if (p0) VMCA(va0, p0); if (p1) VMCA(va1, p1); VF(blk); if (p0) p0 += vl; if (p1) p1 += vl; n -= vl; }
  FENCE();
}
static void run1v(char *blk, long n, unsigned cfg, unsigned long vs1) {
  VSETCFG(cfg); VMCS(vs1, vs1);
  while (n > 0) { long vl; VSETVL(vl, n); VF(blk); n -= vl; }
  FENCE();
}
static double zd[N + 16] __attribute__((aligned(4096))), wd[N + 16] __attribute__((aligned(4096)));
static void run4(char *blk, long n, unsigned cfg, double *p0, double *p1, double *p2, double *p3) {
  VSETCFG(cfg); VMCS(vs1, 0x3ff0000000000000ull);
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, p0); VMCA(va1, p1); VMCA(va2, p2); VMCA(va3, p3); VF(blk); p0 += vl; p1 += vl; p2 += vl; p3 += vl; n -= vl; }
  FENCE();
}
static void run_stride(char *blk, long n, unsigned cfg, void *p0, long stride) {
  VSETCFG(cfg); VMCA(va1, stride);
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, p0); VF(blk); p0 = (char *)p0 + vl * stride; n -= vl; }
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
  TIME("micro_fsqrt_s", run1v(micro_fsqrt_s_vf, N, VCFG(2, 0, 0, 1), 0x4000000040000000ull));   /* 2.0f（两半） */
  TIME("micro_fdiv_s",  run1v(micro_fdiv_s_vf,  N, VCFG(2, 0, 0, 1), 0x4000000040000000ull));
  TIME("micro_fdiv_d",  run1v(micro_fdiv_d_vf,  N, VCFG(2, 0, 0, 1), 0x4000000000000000ull));   /* 2.0 */
  TIME("micro_lstride", run_stride(micro_lstride_vf, N, VCFG(0, 1, 0, 1), xd, 8));
  TIME("micro_sstride", run_stride(micro_sstride_vf, N, VCFG(0, 1, 0, 1), yd, 8));
  TIME("micro_st2",     run1(micro_st2_vf,     N, VCFG(2, 0, 0, 1), xd, yd));
  TIME("micro_ld4st1",  run4(micro_ld4st1_vf,  N, VCFG(4, 0, 0, 1), xd, yd, zd, wd));
  printf("DONE\n");
  return 0;
}
