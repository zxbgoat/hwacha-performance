// 探测 L1D 状态对向量访存的影响：标量核读过/写过的数据，随后向量 load/store 的代价
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
static volatile double sink;
static void run1(char *blk, long n, double *p0) {
  VSETCFG(VCFG(1, 0, 0, 1));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, p0); VF(blk); p0 += vl; n -= vl; }
  FENCE();
}
static void scalar_read(double *p, long n) { double s = 0; for (long i = 0; i < n; i++) s += p[i]; sink = s; }
static void scalar_write(double *p, long n) { for (long i = 0; i < n; i++) p[i] = i; }
#define T(tag, call) do { unsigned long c0 = cyc(); call; unsigned long c1 = cyc(); printf("PROBE %s %lu\n", tag, c1 - c0); } while (0)
int main(void) {
  scalar_write(xd, N); scalar_write(yd, N);
  T("store_after_scalar_write", run1(micro_store_vf, N, yd));
  T("store_warm", run1(micro_store_vf, N, yd));
  T("store_warm", run1(micro_store_vf, N, yd));
  scalar_read(yd, N);
  T("store_after_scalar_read", run1(micro_store_vf, N, yd));
  T("store_warm", run1(micro_store_vf, N, yd));
  scalar_read(yd, N / 4);                                   // 只读前 8 KB
  T("store_after_scalar_read_8KB", run1(micro_store_vf, N, yd));
  T("store_warm", run1(micro_store_vf, N, yd));
  T("load_warm", run1(micro_load_vf, N, xd));
  T("load_warm", run1(micro_load_vf, N, xd));
  scalar_read(xd, N);
  T("load_after_scalar_read", run1(micro_load_vf, N, xd));
  T("load_warm", run1(micro_load_vf, N, xd));
  scalar_write(xd, N);
  T("load_after_scalar_write", run1(micro_load_vf, N, xd));
  T("load_warm", run1(micro_load_vf, N, xd));
  printf("DONE\n");
  return 0;
}
