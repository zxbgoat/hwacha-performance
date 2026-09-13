// RTL 校准基准：把 kernels/*.S 里的向量取指块原样汇编进来，控制线程在这里用内联汇编实现，
// 用 rdcycle 包住整个 stripmine 循环（含结尾 fence），与 hwacha-compiler/test/bench 的测法一致。
#include <stdio.h>
#include <stdint.h>
#include "util.h"

#ifndef N
#define N 4096
#endif
#define VCFG(v64, v32, v16, vp) (((v64) & 0x1ff) | (((vp) & 0x1f) << 9) | (((v32) & 0x1ff) << 14) | (((v16) & 0x1ff) << 23))

extern char vvadd_vf[], saxpy_vf[], daxpy_vf[], csaxpy_vf[], sfilter_vf[], gather_vf[], dgemm_vf[], fma_peak_vf[];

static inline unsigned long cycles_now(void) { unsigned long c; asm volatile("rdcycle %0" : "=r"(c)); return c; }
#define VSETCFG(c)      asm volatile("vsetcfg %0" :: "r"((unsigned long)(c)))
#define VSETVL(vl, n)   asm volatile("vsetvl %0, %1" : "=r"(vl) : "r"(n))
#define VMCA(r, v)      asm volatile("vmca " #r ", %0" :: "r"((unsigned long)(v)))
#define VMCS(r, v)      asm volatile("vmcs " #r ", %0" :: "r"((unsigned long)(v)))
#define VF(blk)         asm volatile("vf 0(%0)" :: "r"(blk))
#define FENCE()         asm volatile("fence" ::: "memory")

static double  xd[N + 16], yd[N + 16], refd[N + 16];
static float   xf[N + 16], yf[N + 16], reff[N + 16], srcf[N + 16], dstf[N + 16];
static uint8_t cond[N + 16];
static int64_t idx[N + 16]; static double table[4096], outd[N + 16];
static double  Bmat[16384 + 64];
static unsigned st = 7;
static double frand(void) { st = st * 1103515245u + 12345u; return ((int)(st >> 8) % 2000 - 1000) / 100.0; }
static uint32_t f2u(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static uint64_t d2u(double d) { union { double d; uint64_t u; } c; c.d = d; return c.u; }

/* ---- 控制线程 ---- */
static void run_vvadd(long n, double *x, double *y) {
  VSETCFG(VCFG(2, 0, 0, 1));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, x); VMCA(va1, y); VF(vvadd_vf); x += vl; y += vl; n -= vl; }
  FENCE();
}
static void run_saxpy(long n, float a, float *x, float *y) {
  VSETCFG(VCFG(0, 2, 0, 1)); VMCS(vs1, f2u(a));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, x); VMCA(va1, y); VF(saxpy_vf); x += vl; y += vl; n -= vl; }
  FENCE();
}
static void run_daxpy(long n, double a, double *x, double *y) {
  VSETCFG(VCFG(2, 0, 0, 1)); VMCS(vs1, d2u(a));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, x); VMCA(va1, y); VF(daxpy_vf); x += vl; y += vl; n -= vl; }
  FENCE();
}
static void run_csaxpy(long n, float a, uint8_t *c, float *x, float *y) {
  VSETCFG(VCFG(0, 2, 0, 1)); VMCS(vs1, f2u(a));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, c); VMCA(va1, x); VMCA(va2, y); VF(csaxpy_vf); c += vl; x += vl; y += vl; n -= vl; }
  FENCE();
}
static void run_sfilter(long n, float *src, float *dst, float m0, float m1, float m2) {
  VSETCFG(VCFG(0, 6, 0, 1)); VMCS(vs1, f2u(m0)); VMCS(vs2, f2u(m1)); VMCS(vs3, f2u(m2));
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, src); VMCA(va1, src + 1); VMCA(va2, src + 2); VMCA(va3, dst); VF(sfilter_vf); src += vl; dst += vl; n -= vl; }
  FENCE();
}
static void run_gather(long n, int64_t *idx, double *tab, double *out) {
  VSETCFG(VCFG(3, 0, 0, 1)); VMCS(vs1, tab);
  while (n > 0) { long vl; VSETVL(vl, n); VMCA(va0, idx); VMCA(va1, out); VF(gather_vf); idx += vl; out += vl; n -= vl; }
  FENCE();
}
static void run_dgemm(long n, double *B) {
  // 8 个 C 向量常驻 VRF，每次 vf 流入两行 B，A 的 8 个标量在 vs1..vs8；B 块 128 KB，越界回绕
  VSETCFG(VCFG(10, 0, 0, 1));
  for (int i = 1; i <= 8; i++) { double a = 0.5 + i; VMCS(vs1, d2u(a)); }  /* 占位：只需发 8 条 vmcs */
  long off = 0;
  while (n > 0) {
    long vl; VSETVL(vl, n);
    VMCS(vs1, d2u(1.5)); VMCS(vs2, d2u(1.5)); VMCS(vs3, d2u(1.5)); VMCS(vs4, d2u(1.5));
    VMCS(vs5, d2u(1.5)); VMCS(vs6, d2u(1.5)); VMCS(vs7, d2u(1.5)); VMCS(vs8, d2u(1.5));
    VMCA(va1, B + off); VMCA(va3, B + 8192 + off); VF(dgemm_vf);
    off = (off + vl) % 8192; n -= vl;
  }
  FENCE();
}
static void run_fma_peak(long n) {
  // 与模型一致：按 n 个元素 stripmine（最后一块可能不满）
  VSETCFG(VCFG(6, 0, 0, 1)); VMCS(vs1, d2u(1.0)); VMCS(vs2, d2u(1.0)); VMCS(vs3, d2u(1.0));
  while (n > 0) { long vl; VSETVL(vl, n); VF(fma_peak_vf); n -= vl; }
  FENCE();
}

static int verify_f(const char *tag, const float *got, const float *want, long n) {
  int bad = 0;
  for (long i = 0; i < n; i++) { float d = got[i] - want[i]; if (d < 0) d = -d; float m = want[i] < 0 ? -want[i] : want[i]; if (d > 1e-4f * (m + 1)) bad++; }
  if (bad) printf("  !! %s: %d mismatches\n", tag, bad);
  return bad;
}
static int verify_d(const char *tag, const double *got, const double *want, long n) {
  int bad = 0;
  for (long i = 0; i < n; i++) { double d = got[i] - want[i]; if (d < 0) d = -d; double m = want[i] < 0 ? -want[i] : want[i]; if (d > 1e-9 * (m + 1)) bad++; }
  if (bad) printf("  !! %s: %d mismatches\n", tag, bad);
  return bad;
}
#define TIME(tag, call, chk) do { unsigned long c0 = cycles_now(); call; unsigned long c1 = cycles_now(); unsigned long cyc = c1 - c0; \
  int bad = (chk); printf("RESULT %s %lu cycles %lu elems%s\n", tag, cyc, (unsigned long)N, bad ? " MISMATCH" : ""); \
  c0 = cycles_now(); call; c1 = cycles_now(); cyc = c1 - c0; \
  printf("RESULT %s_warm %lu cycles %lu elems\n", tag, cyc, (unsigned long)N); \
  c0 = cycles_now(); call; c1 = cycles_now(); cyc = c1 - c0; \
  printf("RESULT %s_warm2 %lu cycles %lu elems\n", tag, cyc, (unsigned long)N); } while (0)

int main(void) {
  int fail = 0;
  printf("N=%d\n", N);
  for (int i = 0; i < N; i++) { xd[i] = frand(); yd[i] = frand(); refd[i] = xd[i] + yd[i]; }
  TIME("vvadd", run_vvadd(N, xd, yd), (fail |= verify_d("vvadd", yd, refd, N)));

  const float a = 1.5f;
  for (int i = 0; i < N; i++) { xf[i] = (float)frand(); yf[i] = (float)frand(); reff[i] = a * xf[i] + yf[i]; }
  TIME("saxpy", run_saxpy(N, a, xf, yf), (fail |= verify_f("saxpy", yf, reff, N)));

  for (int i = 0; i < N; i++) { xd[i] = frand(); yd[i] = frand(); refd[i] = 1.5 * xd[i] + yd[i]; }
  TIME("daxpy", run_daxpy(N, 1.5, xd, yd), (fail |= verify_d("daxpy", yd, refd, N)));

  for (int i = 0; i < N; i++) { cond[i] = (i * 7) % 3 != 0; xf[i] = (float)frand(); yf[i] = (float)frand(); reff[i] = cond[i] ? a * xf[i] + yf[i] : yf[i]; }
  TIME("csaxpy", run_csaxpy(N, a, cond, xf, yf), (fail |= verify_f("csaxpy", yf, reff, N)));

  for (int i = 0; i < N + 2; i++) srcf[i] = (float)frand();
  for (int i = 0; i < N; i++) { reff[i] = 0.25f * srcf[i] + 0.5f * srcf[i + 1] + 0.25f * srcf[i + 2]; dstf[i] = -1; }
  TIME("sfilter", run_sfilter(N, srcf, dstf, 0.25f, 0.5f, 0.25f), (fail |= verify_f("sfilter", dstf, reff, N)));

  for (int i = 0; i < 4096; i++) table[i] = frand();
  for (int i = 0; i < N; i++) { long k = ((long)i * 7919) % 4096; idx[i] = k * 8; refd[i] = table[k]; outd[i] = -1; }
  TIME("gather", run_gather(N, idx, table, outd), (fail |= verify_d("gather", outd, refd, N)));

  for (int i = 0; i < 16384; i++) Bmat[i] = frand();
  TIME("dgemm_opt", run_dgemm(N, Bmat), 0);
  TIME("fma_peak", run_fma_peak(N), 0);

  printf("%s\n", fail ? "VERIFY FAILED" : "ALL VERIFIED");
  return fail;
}
