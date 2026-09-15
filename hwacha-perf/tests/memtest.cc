// 存储系统独立测试：顺序流与随机访问，检查带宽/延迟量级与协议无死锁
#include "mem.hh"
#include <cstdio>
#include <random>

using namespace sim;
using namespace mem;

struct Requester : ClockedObject {
    struct Port : RequestPort {
        Requester &r;
        Port(std::string n, Requester &o) : RequestPort(std::move(n), &o), r(o) {}
        bool recvTimingResp(Packet *pkt) override { return r.recvResp(pkt); }
        void recvReqRetry() override { r.blocked = false; r.issue(); }
    };
    Port port;
    EventFunctionWrapper tick;
    uint64_t issued = 0, done = 0, total, maxOut, inflight = 0;
    bool blocked = false, random;
    Addr base, span;
    std::mt19937_64 rng{7};
    Tick totLat = 0;
    Requester(std::string n, uint64_t tot, uint64_t maxo, bool rnd, Addr span_)
        : ClockedObject(std::move(n), 1000), port(name() + ".port", *this), tick([this] { issue(); }, name() + ".tick"),
          total(tot), maxOut(maxo), random(rnd), base(0x80000000), span(span_) {}
    void startup() override { mainEventQueue().schedule(&tick, clockEdge(1)); }
    void issue() {
        while (issued < total && inflight < maxOut && !blocked) {
            Addr a = random ? base + (rng() % (span / 16)) * 16 : base + (issued * 16) % span;
            auto *p = new Packet(Packet::ReadReq, a, 16);
            if (!port.sendTimingReq(p)) { delete p; blocked = true; return; }
            ++issued; ++inflight;
        }
        if (issued < total && !tick.scheduled() && !blocked) mainEventQueue().schedule(&tick, clockEdge(1));
    }
    bool recvResp(Packet *pkt) {
        totLat += curTick() - pkt->created;
        delete pkt; --inflight; ++done;
        if (!tick.scheduled() && !blocked) mainEventQueue().schedule(&tick, clockEdge(1));
        return true;
    }
};

static double run(bool random, Addr span, uint64_t total, const char *label) {
    // 重新构造一套系统（对象通过 SimObject::all() 注册，测试内简单泄漏即可）
    size_t firstNew = SimObject::all().size();
    Xbar::P xp{1, 4}; xp.widthBytes = 16; xp.interleaveBytes = 64;
    auto *xbar = new Xbar(std::string(label) + ".xbar", 1000, xp);
    Xbar::P mp{4, 2}; mp.widthBytes = 16; mp.interleaveBytes = 64;
    auto *membus = new Xbar(std::string(label) + ".membus", 1000, mp);
    L2Bank::P lp; lp.sizeBytes = 256 * 1024;
    std::vector<L2Bank *> banks;
    for (int i = 0; i < 4; ++i) {
        banks.push_back(new L2Bank(std::string(label) + ".l2" + std::to_string(i), 1000, lp));
        xbar->memSide(i).bind(banks.back()->cpuSide());
        banks.back()->memSide().bind(membus->cpuSide(i));
    }
    DRAMCtrl::P dp;
    std::vector<DRAMCtrl *> drams;
    for (int i = 0; i < 2; ++i) {
        drams.push_back(new DRAMCtrl(std::string(label) + ".dram" + std::to_string(i), 1072, dp, i));
        membus->memSide(i).bind(drams.back()->port());
    }
    auto *req = new Requester(std::string(label) + ".req", total, 64, random, span);
    req->port.bind(xbar->cpuSide(0));
    for (size_t i = firstNew; i < SimObject::all().size(); ++i) SimObject::all()[i]->regStats();
    Tick t0 = curTick();
    for (size_t i = firstNew; i < SimObject::all().size(); ++i) SimObject::all()[i]->startup();
    auto &eq = mainEventQueue();
    while (req->done < total && eq.serviceOne()) {}
    Tick dt = curTick() - t0;
    double bw = double(total * 16) / (dt / 1000.0);
    printf("%-10s bytes=%llu cycles=%llu  BW=%.2f B/cycle  avg latency=%.1f cycles\n", label,
           (unsigned long long)(total * 16), (unsigned long long)(dt / 1000), bw, double(req->totLat) / total / 1000.0);
    return bw;
}

int main() {
    double seq = run(false, 8 << 20, 200000, "seq8MB");     // 超过 L2：DRAM 流式
    double hit = run(false, 64 << 10, 200000, "seq64KB");    // L2 驻留：命中带宽
    double rnd = run(true, 8 << 20, 50000, "rand8MB");
    int fail = 0;
    if (!(seq > 5.0 && seq < 16.0)) { printf("FAIL: streaming bandwidth out of range\n"); fail = 1; }
    if (!(hit > 8.0)) { printf("FAIL: L2 hit bandwidth too low\n"); fail = 1; }
    if (!(rnd < seq)) { printf("FAIL: random should be slower than streaming\n"); fail = 1; }
    printf(fail ? "memtest FAILED\n" : "memtest OK\n");
    return fail;
}
