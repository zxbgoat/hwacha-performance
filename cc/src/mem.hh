// 存储系统：Packet / Port 时序协议（参照 gem5 classic memory system）、交叉开关、L2 bank、DRAM 控制器
#pragma once
#include "sim.hh"
#include <deque>
#include <list>
#include <unordered_map>

namespace mem {
using sim::Tick; using sim::Cycles;
using Addr = uint64_t;

// ---------------------------------------------------------------- Packet
struct Packet {
    enum Cmd { ReadReq, WriteReq, AtomicReq, PrefetchReq, ReadResp, WriteResp, AtomicResp, PrefetchResp };
    struct SenderState { virtual ~SenderState() = default; };

    Cmd cmd;
    Addr addr;
    unsigned size;
    uint64_t id;
    Tick created;
    bool noResp = false;                         // 写回等不需要应答的请求
    std::vector<SenderState *> senderStates;   // 栈：每一层把自己的状态压入，响应时弹出

    Packet(Cmd c, Addr a, unsigned sz);
    ~Packet();
    bool isRead() const { return cmd == ReadReq || cmd == AtomicReq || cmd == PrefetchReq; }
    bool isWrite() const { return cmd == WriteReq || cmd == AtomicReq; }
    bool isPrefetch() const { return cmd == PrefetchReq; }
    bool isRequest() const { return cmd <= PrefetchReq; }
    bool isResponse() const { return !isRequest(); }
    bool needsResponse() const { return isRequest(); }
    void makeResponse() {
        cmd = cmd == ReadReq ? ReadResp : cmd == WriteReq ? WriteResp : cmd == AtomicReq ? AtomicResp : PrefetchResp;
    }
    void pushSenderState(SenderState *s) { senderStates.push_back(s); }
    SenderState *popSenderState() { auto *s = senderStates.back(); senderStates.pop_back(); return s; }
    static uint64_t nextId;
};

// ------------------------------------------------------------------ Ports
class ResponsePort;
class RequestPort {
public:
    RequestPort(std::string n, sim::SimObject *owner) : _name(std::move(n)), _owner(owner) {}
    virtual ~RequestPort() = default;
    void bind(ResponsePort &p);
    bool sendTimingReq(Packet *pkt);
    void sendRespRetry();
    virtual bool recvTimingResp(Packet *pkt) = 0;
    virtual void recvReqRetry() = 0;
    const std::string &name() const { return _name; }
    bool isConnected() const { return _peer != nullptr; }
protected:
    std::string _name;
    sim::SimObject *_owner;
    ResponsePort *_peer = nullptr;
};

class ResponsePort {
public:
    ResponsePort(std::string n, sim::SimObject *owner) : _name(std::move(n)), _owner(owner) {}
    virtual ~ResponsePort() = default;
    bool sendTimingResp(Packet *pkt);
    void sendReqRetry();
    virtual bool recvTimingReq(Packet *pkt) = 0;
    virtual void recvRespRetry() = 0;
    const std::string &name() const { return _name; }
protected:
    friend class RequestPort;
    std::string _name;
    sim::SimObject *_owner;
    RequestPort *_peer = nullptr;
};

// ------------------------------------------------------------------ Xbar
// 非一致性交叉开关：请求按地址交错路由到 mem 侧端口，响应按发送者回到 cpu 侧端口。
// 每个目的端口一条 layer，占用 = 包大小 / 位宽（周期），繁忙时对源端口 retry（gem5 Layer 语义）。
class Xbar : public sim::ClockedObject {
public:
    struct P {
        int nCpuPorts, nMemPorts;
        unsigned widthBytes = 16;
        unsigned interleaveBytes = 64;
        Cycles frontendLatency = 0;
    };
    Xbar(std::string name, Tick period, P p);
    ResponsePort &cpuSide(int i) { return *_cpu[i]; }
    RequestPort &memSide(int i) { return *_mem[i]; }
    int route(Addr a) const { return int((a / _p.interleaveBytes) % _p.nMemPorts); }
    void regStats() override;

    struct Layer {
        enum State { IDLE, BUSY, RETRY } state = IDLE;
        std::deque<int> waiting;
        int retryPort = -1;
        sim::EventFunctionWrapper *release = nullptr;
        Tick busyUntil = 0;
        std::function<void(int)> sendRetry;
    };
private:
    struct CpuPort : ResponsePort {
        Xbar &x; int idx;
        CpuPort(std::string n, Xbar &o, int i) : ResponsePort(std::move(n), &o), x(o), idx(i) {}
        bool recvTimingReq(Packet *pkt) override { return x.recvTimingReq(pkt, idx); }
        void recvRespRetry() override { x.recvRespRetry(idx); }
    };
    struct MemPort : RequestPort {
        Xbar &x; int idx;
        MemPort(std::string n, Xbar &o, int i) : RequestPort(std::move(n), &o), x(o), idx(i) {}
        bool recvTimingResp(Packet *pkt) override { return x.recvTimingResp(pkt, idx); }
        void recvReqRetry() override { x.recvReqRetry(idx); }
    };
    struct RouteState : Packet::SenderState { int src; };

    bool recvTimingReq(Packet *pkt, int src);
    bool recvTimingResp(Packet *pkt, int memIdx);
    void recvReqRetry(int memIdx);
    void recvRespRetry(int cpuIdx);
    bool tryLayer(Layer &l, int port);
    void occupy(Layer &l, Cycles cycles);
    void release(Layer &l);
    void layerFailed(Layer &l, int port) { l.state = Layer::RETRY; l.retryPort = port; }

    P _p;
    std::vector<std::unique_ptr<CpuPort>> _cpu;
    std::vector<std::unique_ptr<MemPort>> _mem;
    std::vector<Layer> _reqLayers, _respLayers;
    sim::stats::Scalar *stReqs = nullptr, *stRetries = nullptr, *stLayerWait = nullptr;
};

// --------------------------------------------------------------- L2Bank
class L2Bank : public sim::ClockedObject {
public:
    struct P {
        unsigned sizeBytes = 256 * 1024;
        unsigned ways = 8;
        unsigned lineBytes = 64;
        Cycles tagLatency = 4;
        Cycles dataLatency = 4;
        Cycles responseLatency = 2;
        unsigned mshrs = 16;
        unsigned tgtsPerMshr = 8;
        unsigned writeBuffers = 8;
        bool supportsAtomics = true;
    };
    L2Bank(std::string name, Tick period, P p);
    ResponsePort &cpuSide() { return _cpu; }
    RequestPort &memSide() { return _mem; }
    void regStats() override;

private:
    struct Line { bool valid = false, dirty = false, pending = false, prefetched = false; Addr tag = 0; Tick lastUsed = 0; };
    struct Mshr {
        Addr lineAddr; bool sent = false; bool prefetched = false; bool demandSeen = false;
        std::deque<Packet *> targets;   // 需要响应的目标；预取目标为 nullptr
        Tick allocated;
    };
    struct CpuPort : ResponsePort {
        L2Bank &c;
        CpuPort(std::string n, L2Bank &o) : ResponsePort(std::move(n), &o), c(o) {}
        bool recvTimingReq(Packet *pkt) override { return c.recvTimingReq(pkt); }
        void recvRespRetry() override { c.recvRespRetry(); }
    };
    struct MemPort : RequestPort {
        L2Bank &c;
        MemPort(std::string n, L2Bank &o) : RequestPort(std::move(n), &o), c(o) {}
        bool recvTimingResp(Packet *pkt) override { return c.recvTimingResp(pkt); }
        void recvReqRetry() override { c.recvReqRetry(); }
    };
    struct QueuedResp { Packet *pkt; Tick ready; };

    bool recvTimingReq(Packet *pkt);
    bool recvTimingResp(Packet *pkt);
    void recvReqRetry();
    void recvRespRetry();
    void handleFill(Packet *fill);
    void queueResponse(Packet *pkt, Tick ready);
    void sendResponses();
    void trySendMem();
    Line *lookup(Addr a, unsigned &setIdx);
    Line *allocate(Addr a);
    Addr lineAddr(Addr a) const { return a & ~Addr(_p.lineBytes - 1); }

    P _p;
    unsigned _nSets;
    std::vector<std::vector<Line>> _sets;
    std::unordered_map<Addr, Mshr *> _mshrs;
    std::deque<Packet *> _memQueue;     // 待发往 DRAM 的填充读 / 写回
    unsigned _wbInQueue = 0;
    std::deque<QueuedResp> _respQueue;
    bool _respBlocked = false;
    bool _memBlocked = false;
    bool _needCpuRetry = false;
    Tick _tagBusyUntil = 0;
    CpuPort _cpu;
    MemPort _mem;
    sim::EventFunctionWrapper _respEvent, _memEvent, _cpuRetryEvent;
    sim::stats::Scalar *stHits, *stMisses, *stMshrHits, *stPrefetches, *stPrefetchUsed, *stWritebacks,
        *stBlockedMshr, *stBlockedTag, *stAtomics;
};

// -------------------------------------------------------------- DRAMCtrl
// 简化的 gem5 DRAMCtrl：读/写队列、FR-FCFS、开页策略、bank/rank 级 JEDEC 时序约束、刷新。
class DRAMCtrl : public sim::ClockedObject {
public:
    struct P {
        // 器件几何
        unsigned banks = 8, ranks = 1;
        unsigned rowBytes = 4096;       // 页大小
        unsigned burstBytes = 32;       // x32 BL8
        unsigned lineBytes = 64;
        unsigned channelInterleave = 64;
        unsigned nChannels = 2;
        // 时序（单位：tCK）
        unsigned tRCD = 17, tRP = 17, tRAS = 40, tRC = 57, tCL = 14, tCWL = 8, tBURST = 4,
                 tCCD = 4, tRTP = 7, tWR = 14, tWTR = 7, tRTW = 4, tRRD = 10, tFAW = 47,
                 tREFI = 3640, tRFC = 122;
        unsigned frontendLatency = 10, backendLatency = 10;   // 控制器前后端固定延迟（tCK）
        unsigned readQueue = 32, writeQueue = 32, writeHigh = 24, writeLow = 8;
    };
    DRAMCtrl(std::string name, Tick tCK, P p, unsigned channelIdx);
    ResponsePort &port() { return _port; }
    void regStats() override;
    void startup() override;

private:
    struct Req {
        Packet *pkt; Addr addr; unsigned rank, bank; uint64_t row; bool isRead; Tick entry; unsigned bursts;
    };
    struct Bank {
        int64_t openRow = -1;
        Tick actAllowedAt = 0, preAllowedAt = 0, colAllowedAt = 0, rdAllowedAt = 0, wrAllowedAt = 0;
    };
    struct Rank {
        std::vector<Bank> banks;
        std::deque<Tick> actTicks;      // tFAW 窗口
        Tick actAllowedAt = 0;          // tRRD
        Tick refreshUntil = 0;
        Tick nextRefresh = 0;
    };
    struct Port : ResponsePort {
        DRAMCtrl &d;
        Port(std::string n, DRAMCtrl &o) : ResponsePort(std::move(n), &o), d(o) {}
        bool recvTimingReq(Packet *pkt) override { return d.recvTimingReq(pkt); }
        void recvRespRetry() override { d.recvRespRetry(); }
    };

    bool recvTimingReq(Packet *pkt);
    void recvRespRetry();
    void decode(Addr a, unsigned &rank, unsigned &bank, uint64_t &row) const;
    void processNextReq();
    bool chooseNext(std::deque<Req> &q, size_t &idx, Tick &earliest);
    Tick estimateCol(const Req &r, bool &rowHit) const;
    Tick doAccess(Req &r);   // 发出命令，返回数据传输完成时刻
    void sendResponses();
    void refreshTick();

    P _p;
    unsigned _chan;
    std::vector<Rank> _ranks;
    std::deque<Req> _readQ, _writeQ;
    struct Resp { Packet *pkt; Tick ready; };
    std::deque<Resp> _respQ;
    bool _respBlocked = false;
    bool _retryRead = false, _retryWrite = false;
    bool _writeMode = false;
    Tick _nextColAllowedAt = 0, _lastCmdAt = 0;
    Tick _lastReadEnd = 0, _lastWriteEnd = 0;
    Port _port;
    sim::EventFunctionWrapper _nextReqEvent, _respEvent, _refreshEvent;
    sim::stats::Scalar *stReads, *stWrites, *stRowHits, *stRowMisses, *stReadBytes, *stWriteBytes,
        *stTotReadLat, *stQueueLat, *stBusBusy, *stRetries;
};

}  // namespace mem
