// 探针 6：同一个 store/load/copy 微内核，在一块 512 KB、64 KB 对齐的缓冲区里改变数组起点相对 64 KB（L2 set 周期）的偏移，
// 看访存吞吐随布局的变化（定位 InclusiveCache 同 set 排队 / bank 冲突）。每个偏移三次计时。
#include <stdio.h>
#include <stdint.h>
#include "util.h"
#define N 4096
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))
extern char micro_load_vf[], micro_store_vf[], micro_copy_vf[];
static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")
static double buf[65536] __attribute__((aligned(65536)));   /* 512 KB */
static void run1(char *blk, long n, unsigned cfg, double *p0, double *p1) {
  VSETCFG(cfg); VMCS(vs1, 0x3ff0000000000000ull);
  while (n > 0) { long vl; VSETVL(vl, n); if (p0) VMCA(va0, p0); if (p1) VMCA(va1, p1); VF(blk); if (p0) p0 += vl; if (p1) p1 += vl; n -= vl; }
  FENCE();
}
#define TIME3(tag, call) do { unsigned long c0, c1; c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); printf("RESULT %s_warm2 %lu cycles %lu elems\n", tag, c1 - c0, (unsigned long)N); } while (0)
int main(void) {
  for (int i = 0; i < 65536; i++) buf[i] = i;
  static const long offs[] = {0, 64, 1024, 4096, 8192, 16384, 32768, 49152};   /* 字节偏移（相对 64 KB 边界） */
  for (int k = 0; k < 8; k++) {
    double *p = (double *)((char *)buf + offs[k]);
    printf("OFF %ld\n", offs[k]); TIME3("store_off", run1(micro_store_vf, N, VCFG(1, 0, 0, 1), p, 0));
    TIME3("load_off", run1(micro_load_vf,  N, VCFG(1, 0, 0, 1), p, 0));
  }
  /* copy：源固定在 0，目的相对源的距离变化（同 set 冲突：距离为 64 KB 的整数倍） */
  static const long dist[] = {32768 + 4096, 65536, 65536 + 4096, 131072, 131072 + 1024};
  for (int k = 0; k < 5; k++) {
    printf("DIST %ld\n", dist[k]); TIME3("copy_dist", run1(micro_copy_vf, N, VCFG(1, 0, 0, 1), buf, (double *)((char *)buf + dist[k])));
  }
  printf("DONE\n"); return 0;
}
