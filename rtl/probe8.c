// 探针 8：索引访存（gather）在多 lane + 多 bank 下的对照（配合 +hwacha_tl_trace 看 bank 冲突）
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define N 4096
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char gather_vf[];
static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static int64_t idx[N + 16] __attribute__((aligned(4096))); static double table[4096] __attribute__((aligned(4096))), outd[N + 16] __attribute__((aligned(4096)));
static void run_gather(long n, int64_t *ix, double *tab, double *out) {
  VSETCFG(VCFG(3, 0, 0, 1)); VMCS(vs1, tab);
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, ix); VMCA(va1, out); VF(gather_vf); ix += vl; out += vl; n -= vl; }
  FENCE();
}
#define TIME3(tag, call) do { unsigned long c0, c1; c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm2 %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); } while (0)
int main(void) {
  for (int i = 0; i < 4096; i++) table[i] = i;
  for (int i = 0; i < N; i++) idx[i] = (((long)i * 7919) % 4096) * 8;
  TIME3("gather", run_gather(N, idx, table, outd));
  for (int i = 0; i < N; i++) idx[i] = (long)i * 8;      /* 顺序索引：与单位步长 load 对照 */
  TIME3("gather_seq", run_gather(N, idx, table, outd));
  printf("DONE\n"); return 0;
}
