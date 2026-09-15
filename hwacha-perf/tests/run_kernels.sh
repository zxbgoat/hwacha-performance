#!/bin/bash
# 回归：所有示例内核都能在限定时间内完成，且关键性质成立
set -u
SIM="$1"; ROOT="$2"; fail=0
run() { timeout 300 "$SIM" run "$ROOT/kernels/$1" --quiet "${@:2}" --json 2>/dev/null; }
get() { python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"; }
for k in vvadd saxpy daxpy csaxpy dgemm_opt sfilter gather fma_peak; do
  out=$(run $k.S --n 8192) || { echo "FAIL: $k did not complete"; fail=1; continue; }
  echo "$k cycles=$(echo "$out" | get cycles) gflops=$(echo "$out" | get gflops)"
done
# FMA 峰值：双 FMA 簇 → 8 GFLOPS @ 1 GHz
g=$(run fma_peak.S | get gflops); python3 -c "import sys; sys.exit(0 if abs($g-8.0)<0.3 else 1)" || { echo "FAIL: fma_peak gflops=$g"; fail=1; }
# VRU 对延迟受限的流式内核有效（限制在途 beat 数使其不再被 DRAM 带宽而是延迟限制）
on=$(run daxpy.S --n 16384 --set n_vmt_entries=16 | get cycles); off=$(run daxpy.S --n 16384 --set n_vmt_entries=16 --set build_vru=false | get cycles)
[ "$on" -lt "$off" ] || { echo "FAIL: vru on=$on off=$off"; fail=1; }
# 带宽受限时 VRU 不应明显变差
on=$(run daxpy.S --n 16384 | get cycles); off=$(run daxpy.S --n 16384 --set build_vru=false | get cycles)
python3 -c "import sys; sys.exit(0 if $on < 1.05*$off else 1)" || { echo "FAIL: vru overhead on=$on off=$off"; fail=1; }
# 多 lane 对计算受限内核有效
l1=$(run dgemm_opt.S --n 65536 --lanes 1 | get cycles); l2=$(run dgemm_opt.S --n 65536 --lanes 2 | get cycles)
python3 -c "import sys; sys.exit(0 if $l2 < 0.7*$l1 else 1)" || { echo "FAIL: lanes 1=$l1 2=$l2"; fail=1; }
# 混合精度：saxpy HVL 翻倍
m1=$(run saxpy.S --n 4096 | get max_vlen); m2=$(run saxpy.S --n 4096 --set conf_prec=true | get max_vlen)
[ "$m2" -eq $((2*m1)) ] || { echo "FAIL: conf_prec maxvl $m1 -> $m2"; fail=1; }
[ $fail -eq 0 ] && echo "run_kernels OK"
exit $fail
