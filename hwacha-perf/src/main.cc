// hwacha-sim：gem5 风格事件驱动的 Hwacha 周期级性能模型
#include "hwacha.hh"
#include "trace.hh"
#include <cstring>
#include <iostream>

using namespace sim;

static void usage() {
    std::cerr << "usage: hwacha-sim run <kernel.S> [--n N] [--lanes L] [--config cfg.json] [--set key=val]... [--trace spike.log [--trace-range lo:hi] [--trace-base pc]] [--json] [--stats]\n"
                 "  --set 前缀 mem. 表示存储系统参数（如 mem.l2_banks=4 mem.dram_tck_ps=1072）\n";
}

int main(int argc, char **argv) {
    if (argc < 3 || std::strcmp(argv[1], "run") != 0) { usage(); return 1; }
    std::string kernelPath = argv[2];
    Params hwP, memP;
    int64_t nOverride = -1;
    bool json = false, dumpStats = false;
    std::string tracePath; uint64_t traceLo = 0, traceHi = ~0ull, traceBase = 0; size_t traceB0 = 0, traceB1 = ~size_t(0);
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) fatal("missing value for " + a); return argv[++i]; };
        if (a == "--n") nOverride = std::stoll(next());
        else if (a == "--lanes") hwP.set("n_lanes", next());
        else if (a == "--config") {
            Json j = Json::parseFile(next());
            if (auto *h = j.get("hwacha")) hwP.loadJson(*h);
            if (auto *m = j.get("memory")) memP.loadJson(*m);
        } else if (a == "--set") {
            std::string kv = next(); auto eq = kv.find('=');
            if (eq == std::string::npos) fatal("bad --set " + kv);
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k.rfind("mem.", 0) == 0) memP.set(k.substr(4), v); else hwP.set(k, v);
        } else if (a == "--trace") tracePath = next();
        else if (a == "--trace-range") {
            std::string r = next(); auto c = r.find(':');
            traceLo = std::stoull(r.substr(0, c), nullptr, 16); traceHi = std::stoull(r.substr(c + 1), nullptr, 16);
        } else if (a == "--trace-base") traceBase = std::stoull(next(), nullptr, 16);
        else if (a == "--trace-blocks") {
            std::string r = next(); auto c = r.find(':');
            traceB0 = std::stoul(r.substr(0, c)); traceB1 = std::stoul(r.substr(c + 1));
        } else if (a == "--json") json = true;
        else if (a == "--stats") dumpStats = true;
        else if (a == "--quiet") verboseWarn = false;
        else { usage(); return 1; }
    }

    hw::Kernel kernel = hw::loadKernel(kernelPath);
    hw::HwachaParams hp = hw::HwachaParams::from(hwP);
    uint64_t n = nOverride >= 0 ? (uint64_t)nOverride : kernel.n;
    Tick corePeriod = hp.period();

    // ---- 存储系统参数（键名与 Python 模型的 MemoryConfig 一致，另加 DRAM 时序键） ----
    unsigned l2Banks = (unsigned)memP.getInt("l2_banks", 4);
    unsigned lineBytes = (unsigned)memP.getInt("line_bytes", 64);
    mem::L2Bank::P lp;
    lp.sizeBytes = (unsigned)memP.getInt("l2_bytes_per_bank", 256 * 1024);
    lp.ways = (unsigned)memP.getInt("l2_ways", 8);
    lp.lineBytes = lineBytes;
    unsigned hitLat = (unsigned)memP.getInt("l2_hit_latency", 24);
    lp.tagLatency = (unsigned)memP.getInt("l2_tag_latency", hitLat / 2);
    lp.dataLatency = (unsigned)memP.getInt("l2_data_latency", hitLat - lp.tagLatency);
    lp.mshrs = (unsigned)memP.getInt("l2_trackers_per_bank", 16);
    lp.supportsAtomics = memP.getBool("l2_supports_amo", true);
    lp.storeCycles = memP.getDouble("l2_store_beat_cycles", 1.0);
    lp.storeSwitch = memP.getDouble("l2_store_switch", 0.0);
    lp.probeCycles = (unsigned)memP.getInt("l1d_probe_cycles", 4);
    lp.partialStoreSwitch = memP.getDouble("l2_partial_store_switch", 0.0);
    {   // "1:0.05,2:0.09,4:0.2,8:0.75,16:0.97"
        std::string tbl = memP.getString("l2_store_conflict", "");
        size_t pos = 0;
        while (pos < tbl.size()) {
            size_t c = tbl.find(':', pos), e = tbl.find(',', pos); if (e == std::string::npos) e = tbl.size();
            if (c == std::string::npos || c > e) break;
            lp.storeConflict.emplace_back((unsigned)std::stoul(tbl.substr(pos, c - pos)), std::stod(tbl.substr(c + 1, e - c - 1)));
            pos = e + 1;
        }
        lp.storeWindow = (unsigned)memP.getInt("l2_store_window", 32);
    }
    unsigned channels = (unsigned)memP.getInt("dram_channels", 2);
    mem::DRAMCtrl::P dp;
    dp.nChannels = channels; dp.lineBytes = lineBytes; dp.channelInterleave = lineBytes;
    dp.banks = (unsigned)memP.getInt("dram_banks", 8);
    dp.rowBytes = (unsigned)memP.getInt("dram_row_bytes", 4096);
    dp.burstBytes = (unsigned)memP.getInt("dram_burst_bytes", 32);
    Tick tCK = (Tick)memP.getInt("dram_tck_ps", 1072);
    auto T = [&](const char *k, unsigned &f) { f = (unsigned)memP.getInt(k, f); };
    T("dram_tRCD", dp.tRCD); T("dram_tRP", dp.tRP); T("dram_tRAS", dp.tRAS); T("dram_tRC", dp.tRC); T("dram_tCL", dp.tCL);
    T("dram_tCWL", dp.tCWL); T("dram_tBURST", dp.tBURST); T("dram_tCCD", dp.tCCD); T("dram_tRTP", dp.tRTP); T("dram_tWR", dp.tWR);
    T("dram_tWTR", dp.tWTR); T("dram_tRTW", dp.tRTW); T("dram_tRRD", dp.tRRD); T("dram_tFAW", dp.tFAW); T("dram_tREFI", dp.tREFI);
    T("dram_tRFC", dp.tRFC); T("dram_frontend_latency", dp.frontendLatency); T("dram_backend_latency", dp.backendLatency);
    T("dram_read_queue", dp.readQueue); T("dram_write_queue", dp.writeQueue);
    // 让 tlb/page 参数对两个模型保持同名
    if (memP.has("tlb_entries")) hp.tlbEntries = (unsigned)memP.getInt("tlb_entries", hp.tlbEntries);
    if (memP.has("tlb_miss_latency")) hp.tlbMissLatency = (unsigned)memP.getInt("tlb_miss_latency", hp.tlbMissLatency);
    if (memP.has("page_bytes")) hp.pageBytes = (unsigned)memP.getInt("page_bytes", hp.pageBytes);

    // ---- 构建系统 ----
    hw::Trace trace;
    const hw::Trace *tracePtr = nullptr;
    if (!tracePath.empty()) {
        trace = hw::Trace::load(tracePath, traceLo, traceHi, traceBase ? traceBase : traceLo);
        if (traceB1 < trace.blocks.size()) trace.blocks.resize(traceB1);
        if (traceB0 > 0) trace.blocks.erase(trace.blocks.begin(), trace.blocks.begin() + std::min(traceB0, trace.blocks.size()));
        if (trace.blocks.empty()) fatal("trace contains no vf blocks in range");
        tracePtr = &trace;
        // 执行驱动时按踪迹决定迭代次数与元素数
        uint64_t total = 0; for (auto &b : trace.blocks) total += b.vl;
        if (nOverride < 0) n = total;
    }
    auto *hwacha = new hw::Hwacha("hwacha", hp, kernel, n, lineBytes, tracePtr);
    unsigned nClients = hp.nLanes + (hp.buildVru ? 1 : 0);
    // rocc_shared_port：所有 lane（与 VRU）经 RoCC 的单个 TileLink 端口进入系统总线（Chipyard 集成方式）；
    // 否则每 lane 一个端口直接进入 L2 交叉开关（论文图 8.2）
    bool sharedPort = memP.getBool("rocc_shared_port", false);
    unsigned nCpuPorts = sharedPort ? 1 : nClients;
    mem::Xbar::P xp{(int)nCpuPorts, (int)l2Banks}; xp.widthBytes = hp.tlDataBytes; xp.interleaveBytes = lineBytes;
    auto *xbar = new mem::Xbar("system.l1_to_l2_xbar", corePeriod, xp);
    mem::Xbar *roccXbar = nullptr;
    if (sharedPort) {
        mem::Xbar::P rp{(int)nClients, 1}; rp.widthBytes = hp.tlDataBytes; rp.interleaveBytes = 1u << 30;
        rp.switchPenalty = memP.getDouble("rocc_switch_penalty", 0);
        roccXbar = new mem::Xbar("system.rocc_port", corePeriod, rp);
        roccXbar->memSide(0).bind(xbar->cpuSide(0));
    }
    mem::Xbar::P mp{(int)l2Banks, (int)channels}; mp.widthBytes = 16; mp.interleaveBytes = lineBytes;
    auto *membus = new mem::Xbar("system.membus", corePeriod, mp);
    std::vector<mem::L2Bank *> l2s;
    for (unsigned i = 0; i < l2Banks; ++i) {
        auto *b = new mem::L2Bank("system.l2.bank" + std::to_string(i), corePeriod, lp);
        xbar->memSide((int)i).bind(b->cpuSide());
        b->memSide().bind(membus->cpuSide((int)i));
        l2s.push_back(b);
    }
    for (unsigned i = 0; i < channels; ++i) {
        auto *d = new mem::DRAMCtrl("system.dram.ch" + std::to_string(i), tCK, dp, i);
        membus->memSide((int)i).bind(d->port());
    }
    mem::Xbar *clientXbar = sharedPort ? roccXbar : xbar;
    for (unsigned l = 0; l < hp.nLanes; ++l) hwacha->lane((int)l).port().bind(clientXbar->cpuSide((int)l));
    if (hp.buildVru) hwacha->vru()->port().bind(clientXbar->cpuSide((int)hp.nLanes));
    for (auto *o : SimObject::all()) o->regStats();
    if (memP.getBool("warm_l2", false) && tracePtr) {
        // 执行驱动：预热踪迹中所有访存触及的行
        for (auto &b : trace.blocks) for (auto &ti : b.instrs) for (auto &[ut, addr] : ti.mem)
            l2s[xbar->route(addr)]->installLine(addr, true);
    } else if (memP.getBool("warm_l2", false)) {
        // 预热规则：被 va 指针引用的数组预热到 (n + offset + 16) 个元素 × 步长；仅被索引访存引用的数组整体预热
        for (auto &[name, arr] : kernel.arrays) {
            uint64_t bytes = 0; bool referenced = false;
            for (auto &[reg, d] : kernel.va) if (d.isPtr && d.array == name) {
                referenced = true;
                bytes = std::max<uint64_t>(bytes, (n + d.offsetElems + 16) * (uint64_t)arr.elem * std::max(1, d.strideElems));
            }
            if (!referenced) bytes = arr.nbytes();
            bytes = std::min<uint64_t>(bytes, arr.nbytes());
            for (uint64_t a = arr.base; a < arr.base + bytes; a += lineBytes)
                l2s[xbar->route(a)]->installLine(a, true);
        }
    }

    // 冷启动（mem.cold_start=true）：标量核初始化后仍留在 L1D 里的脏行（按内核数组列表顺序取最后 l1d_dirty_bytes 字节）
    // 第一次被向量访存触到时要探测 L1D；对应 RTL 的第一次计时，而不是 warm2 稳态
    if (memP.getBool("cold_start", false) && lp.probeCycles) {
        uint64_t budget = (uint64_t)memP.getInt("l1d_dirty_bytes", 16384);
        // 每个数组只算本次运行会用到的那一段（与 warm_l2 的规则相同），从列表末尾往前取
        std::vector<std::pair<uint64_t, uint64_t>> used;   // (起始地址, 字节数)
        for (auto &[name, arr] : kernel.arrays) {
            uint64_t bytes = 0; bool referenced = false;
            for (auto &[reg, d] : kernel.va) if (d.isPtr && d.array == name) {
                referenced = true;
                bytes = std::max<uint64_t>(bytes, (n + d.offsetElems + 16) * (uint64_t)arr.elem * std::max(1, d.strideElems));
            }
            if (!referenced) bytes = arr.nbytes();
            used.emplace_back(arr.base, std::min<uint64_t>(bytes, arr.nbytes()));
        }
        for (auto it = used.rbegin(); it != used.rend() && budget; ++it) {
            uint64_t take = std::min<uint64_t>(budget, it->second);
            for (uint64_t a = it->first + it->second - take; a < it->first + it->second; a += lineBytes)
                l2s[xbar->route(a)]->markProbe(a);
            budget -= take;
        }
    }
    for (auto *o : SimObject::all()) o->startup();
    auto &eq = mainEventQueue();
    while (!hwacha->done()) {
        if (!eq.serviceOne()) fatal("event queue empty before completion");
    }

    if (json) {
        std::cout << hwacha->reportJson();
        if (dumpStats) stats::registry().dumpJson(std::cout);
    } else {
        std::cout << "kernel: " << kernel.name << "   lanes=" << hp.nLanes << " vru=" << (hp.buildVru ? "true" : "false")
                  << " conf_prec=" << (hp.confPrec ? "true" : "false") << "  (events " << eq.numEvents << ")\n";
        std::cout << hwacha->reportText();
        // 汇总 L2 / DRAM
        double hits = 0, misses = 0, mshrHits = 0, pf = 0, pfUsed = 0, wb = 0, rd = 0, wr = 0, rowHit = 0, rowMiss = 0, rlat = 0;
        for (auto *s : stats::registry().all) {
            auto ends = [&](const char *suf) { return s->name.size() >= std::strlen(suf) && s->name.compare(s->name.size() - std::strlen(suf), std::strlen(suf), suf) == 0; };
            if (s->name.rfind("system.l2", 0) == 0) {
                if (ends(".hits")) hits += s->value(); else if (ends(".misses")) misses += s->value();
                else if (ends(".mshr_hits")) mshrHits += s->value(); else if (ends(".prefetches")) pf += s->value();
                else if (ends(".prefetch_used")) pfUsed += s->value(); else if (ends(".writebacks")) wb += s->value();
            } else if (s->name.rfind("system.dram", 0) == 0) {
                if (ends(".reads")) rd += s->value(); else if (ends(".writes")) wr += s->value();
                else if (ends(".row_hits")) rowHit += s->value(); else if (ends(".row_misses")) rowMiss += s->value();
                else if (ends(".tot_read_latency_ticks")) rlat += s->value();
            }
        }
        std::cout << "L2: hits " << hits << ", misses " << misses << ", mshr-hits " << mshrHits << ", prefetches " << pf
                  << " (used " << pfUsed << "), writebacks " << wb << "\n";
        std::cout << "DRAM: reads " << rd << ", writes " << wr << ", row hits " << rowHit << ", row misses " << rowMiss
                  << ", avg read latency " << (rd > 0 ? rlat / rd / corePeriod : 0) << " cycles\n";
        if (dumpStats) { std::cout << "\n---- stats ----\n"; stats::registry().dump(std::cout); }
    }
    return 0;
}
