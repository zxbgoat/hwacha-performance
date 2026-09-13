#include "trace.hh"
#include "sim.hh"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace hw {

static uint64_t hexField(const std::string &line, const char *key) {
    size_t p = line.find(key);
    if (p == std::string::npos) return 0;
    return std::strtoull(line.c_str() + p + std::strlen(key), nullptr, 16);
}
static unsigned decField(const std::string &line, const char *key) {
    size_t p = line.find(key);
    if (p == std::string::npos) return 0;
    return (unsigned)std::strtoul(line.c_str() + p + std::strlen(key), nullptr, 10);
}

Trace Trace::load(const std::string &path, uint64_t lo, uint64_t hi, uint64_t base) {
    std::ifstream f(path);
    if (!f) sim::fatal("cannot open trace " + path);
    Trace t;
    TraceBlock cur;
    bool inBlock = false;
    uint64_t expectPc = 0;
    std::vector<std::pair<unsigned, uint64_t>> pendingMem;
    std::string line;
    auto flush = [&]() {
        if (inBlock && !cur.instrs.empty() && cur.startPc >= lo && cur.startPc < hi) t.blocks.push_back(cur);
        cur = TraceBlock(); inBlock = false;
    };
    while (std::getline(f, line)) {
        if (line.rfind("H: WT ", 0) == 0) {
            TraceInstr ti;
            ti.pc = hexField(line, "pc=");
            ti.inst = hexField(line, "inst=");
            ti.next = hexField(line, "next=");
            ti.vl = decField(line, "vl=");
            size_t ap = line.find("act=");
            std::string act = ap == std::string::npos ? "-" : line.substr(ap + 4);
            while (!act.empty() && (act.back() == '\r' || act.back() == ' ')) act.pop_back();
            // 块边界：上一条是 vstop（next == 0 或未接续）或 pc 不等于上一条的 next
            if (!inBlock || ti.pc != expectPc) { flush(); inBlock = true; cur.startPc = ti.pc; cur.basePc = base ? base : ti.pc; cur.vl = ti.vl; }
            if (act == "-") ti.scalar = true;
            else {
                ti.active.assign(ti.vl, 0);
                for (unsigned i = 0; i < ti.vl; ++i) {
                    char c = i / 4 < act.size() ? act[i / 4] : '0';
                    unsigned nib = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
                    ti.active[i] = (nib >> (i % 4)) & 1;
                }
            }
            ti.mem.swap(pendingMem);
            cur.instrs.push_back(ti);
            expectPc = ti.next;
            // vstop：inst 低 12 位 0xc3f 且高位全零
            if ((ti.inst & 0xfff) == 0xc3f && (ti.inst >> 12) == 0) flush();
        } else if (line.rfind("HMEM:", 0) == 0) {
            uint64_t addr = 0;
            if (line.rfind("HMEM: read from ", 0) == 0) addr = std::strtoull(line.c_str() + 16, nullptr, 16);
            else if (line.rfind("HMEM: write ", 0) == 0) addr = std::strtoull(line.c_str() + 12, nullptr, 16);
            else if (line.rfind("HMEM: rmw ", 0) == 0) addr = std::strtoull(line.c_str() + 10, nullptr, 16);
            else continue;
            unsigned ut = decField(line, "ut=");
            pendingMem.push_back({ut, addr});
        }
    }
    flush();
    return t;
}

void Trace::filter(uint64_t lo, uint64_t hi) {
    std::vector<TraceBlock> keep;
    for (auto &b : blocks) if (b.startPc >= lo && b.startPc < hi) keep.push_back(b);
    blocks.swap(keep);
}

}  // namespace hw
