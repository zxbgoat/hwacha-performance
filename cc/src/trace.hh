// Spike（hlog 构建）指令级踪迹：把 `H: WT pc=… inst=… next=… vl=… act=…` 与 `HMEM: read/write/rmw … ut=…`
// 解析成每次 vf 块执行的动态指令序列，用于执行驱动的时序模拟。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace hw {

struct TraceInstr {
    uint64_t pc = 0, inst = 0, next = 0;
    unsigned vl = 0;
    bool scalar = false;
    std::vector<uint8_t> active;               // 每元素是否活跃（vl 个）
    std::vector<std::pair<unsigned, uint64_t>> mem;   // (元素索引, 地址)，仅访存指令
    unsigned nActive() const { unsigned n = 0; for (auto a : active) n += a; return n; }
    bool taken() const { return next != pc + 8; }
};

struct TraceBlock {                             // 一次 vf 块的执行
    uint64_t startPc = 0;
    unsigned vl = 0;
    std::vector<TraceInstr> instrs;
};

struct Trace {
    std::vector<TraceBlock> blocks;
    // 只保留起始 pc 落在 [lo, hi) 内的块（用 ELF 符号表把内核名映射到地址范围）
    void filter(uint64_t lo, uint64_t hi);
    static Trace load(const std::string &path, uint64_t lo = 0, uint64_t hi = ~0ull);
};

}  // namespace hw
