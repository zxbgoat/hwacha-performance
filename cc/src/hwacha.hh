// Hwacha 微架构模型（gem5 风格 ClockedObject：每周期一个 tick 事件驱动内部流水；访存经 Port/Packet 走事件驱动的存储系统）
#pragma once
#include "isa.hh"
#include "mem.hh"
#include "sim.hh"
#include "trace.hh"
#include <deque>
#include <memory>
#include <random>
#include <set>

namespace hw {
using sim::Tick; using sim::Cycles;
constexpr Cycles NoCycle = ~Cycles(0);

// --------------------------------------------------------------- 参数
struct HwachaParams {
    unsigned nLanes = 1, nBanks = 4, bankWidth = 128, regLen = 64, nSramEntries = 256, nPredEntries = 256;
    unsigned nVectorRegs = 256, nPredRegs = 16, nSeqEntries = 8, nFmaUnits = 2;
    bool confPrec = false;
    unsigned stagesAlu = 1, stagesPlu = 0, stagesIMul = 3, stagesDFma = 4, stagesSFma = 3, stagesHFma = 3, stagesFConv = 2, stagesFCmp = 1;
    unsigned fdivCyclesPerElem = 22, idivCyclesPerElem = 65;
    unsigned cmdqLen = 32, vfFetchLatency = 2, scalarSmuLatency = 30, scalarFpuLatency = 8, scalarMulDivLatency = 8;
    unsigned branchResolveLatency = 4, ctrlCyclesPerIter = 6;
    unsigned branchStripCycles = 4;  // 谓词归约每 strip 占用的周期（RTL 实测 6）
    unsigned nVmtEntries = 64, vmuIssueLatency = 4, vluLatency = 3, vsdqBeats = 8, vldqBeats = 4, vvaqEntries = 4;
    unsigned brqDepth = 4, bwqDepth = 2, tlDataBytes = 16;
    double storeBeatCycles = 1.0;    // 每个 store beat 占用 VMU 端口的周期数（RTL 校准用，可为分数）
    double loadBeatCycles = 1.0;
    unsigned vfBlockOverhead = 0;    // 每个 vf 块额外的固定周期（校准用）
    bool buildVru = true;
    unsigned vruMaxOutstanding = 20, vruEarlyIgnore = 1;
    uint64_t vruMaxRunaheadBytes = 1ull << 24;
    unsigned tlbEntries = 8, tlbMissLatency = 40, pageBytes = 4096;
    double freqGhz = 1.0;
    bool commitLog = false;

    static HwachaParams from(const sim::Params &p);
    unsigned nSlices() const { return bankWidth / regLen; }
    unsigned nStrip() const { return nBanks * nSlices(); }
    unsigned maxVlenPerLane() const { return nBanks * nSramEntries * bankWidth / regLen; }
    Tick period() const { return Tick(1000.0 / freqGhz + 0.5); }
};
unsigned maxVlen(const HwachaParams &p, const VCfg &vc);
unsigned rateOf(const HwachaParams &p, char prec);

// ------------------------------------------------------------- 资源时间线
// 按周期占用的资源（对应展开器 ticker / bank 端口）。用环形数组记录"该槽位被哪一周期占用"。
class Timeline {
public:
    explicit Timeline(size_t depth = 4096) : _ring(depth, NoCycle) {}
    bool free(Cycles c, unsigned dur = 1) const { for (unsigned i = 0; i < dur; ++i) if (_ring[(c + i) % _ring.size()] == c + i) return false; return true; }
    void reserve(Cycles c, unsigned dur = 1) { for (unsigned i = 0; i < dur; ++i) _ring[(c + i) % _ring.size()] = c + i; }
    Cycles firstFree(Cycles c) const { while (!free(c)) ++c; return c; }
private:
    std::vector<Cycles> _ring;
};

class Hwacha;
class Lane;
struct LaneOp;
struct Block { int id; unsigned vl; int pendingOps = 0; bool stopped = false, acked = false; Cycles start = 0; };

// --------------------------------------------------------------- 向量操作
struct VectorOp {
    uint64_t id;
    const Instr *ins;
    Block *block;
    Cycles issueCycle = 0;
    unsigned vl, rate, E;
    unsigned slots;
    std::vector<std::unique_ptr<LaneOp>> lanes;
    struct Dep { VectorOp *op; bool raw, war, waw; };
    std::vector<Dep> deps;
    std::set<std::string> reads, writes;
    bool complete = false;
    Cycles completeCycle = 0;
    uint64_t base = 0, strideBytes = 0;
    const ArrayDef *gather = nullptr;
    bool gatherUnit = false;
    unsigned fmaElems = 0;
    const TraceInstr *trace = nullptr;         // 执行驱动：该指令的活跃掩码与访存地址
    bool activeAt(unsigned e) const { return !trace || trace->scalar || e >= trace->active.size() || trace->active[e]; }
};

struct LaneOp {
    VectorOp *op;
    int lane;
    std::vector<std::pair<unsigned, unsigned>> chunks;   // 每块 ≤ nStrip 个元素的 [e0, e1)
    unsigned nstrips = 0, nextStrip = 0, nextDone = 0;
    std::vector<Cycles> issue, readTime, done, dataReady, addrReady;
    // 访存
    struct Beat { uint64_t addr; std::vector<unsigned> banks; };
    std::vector<std::vector<Beat>> beats;
    std::vector<unsigned> beatsReturned, beatsWritten;
    std::vector<Cycles> lastWrite;
    unsigned stripPtr = 0, beatPtr = 0, stripsSent = 0;
    Cycles vmuStart = 0;
    bool finished = false;
    Cycles finishCycle = NoCycle;

    unsigned nElems(unsigned k) const;
    unsigned nActive(unsigned k) const;
    std::pair<unsigned, unsigned> stripRange(unsigned k) const;
    unsigned stripOfChunk(unsigned chunkIdx) const { return chunkIdx / op->rate; }
};

// ------------------------------------------------------------------ Lane
class Lane {
public:
    Lane(Hwacha &h, int id);
    // 每周期阶段
    void memCompletionStep(Cycles now);
    void vmuStep(Cycles now);
    bool schedule(Cycles now);
    void retireStep(Cycles now);
    void vluStep(Cycles now);
    // VMU 端口
    bool recvTimingResp(mem::Packet *pkt);
    void recvReqRetry();
    mem::RequestPort &port() { return _port; }

    std::vector<LaneOp *> ops;
    std::deque<LaneOp *> vmuQueue;
    std::string lastReason = "empty", vmuReason = "idle";
    // 统计
    uint64_t stripsIssued = 0, loadBeats = 0, storeBeats = 0, readPortBusy = 0, writePortBusy = 0, beatsReturned = 0;
    unsigned outstanding() const { return _outstanding; }
    size_t vldqSize() const { return _vldq.size(); }
    unsigned vsdqUsed() const { return _vsdqUsed; }
    bool portBlocked() const { return _portBlocked; }
    std::map<std::string, uint64_t> fuBusy;

private:
    struct Port : mem::RequestPort {
        Lane &l;
        Port(std::string n, Lane &o, sim::SimObject *owner) : RequestPort(std::move(n), owner), l(o) {}
        bool recvTimingResp(mem::Packet *pkt) override { return l.recvTimingResp(pkt); }
        void recvReqRetry() override { l.recvReqRetry(); }
    };
    struct BeatState : mem::Packet::SenderState { LaneOp *lo; unsigned strip; unsigned beat; };
    struct VldqEntry { LaneOp *lo; unsigned strip, beat; Cycles arrived; };

    const char *hazard(LaneOp &b, unsigned k, Cycles now, Cycles writeTime, bool checkWaw);
    bool loadWriteGate(LaneOp &b, unsigned k, Cycles now);
    const char *tryIssue(LaneOp &b, unsigned k, Cycles now);
    std::vector<unsigned> stripsCovering(const LaneOp &a, unsigned e0, unsigned e1) const;

    Hwacha &_h;
    const HwachaParams &_p;
    int _id;
    Timeline _readPort, _predRead, _predWrite;
    std::vector<Timeline> _bankWrite;                  // 每 bank 写口
    std::map<std::string, Timeline> _units;            // fma0 fma1 imul fconv fcmp vqu vgu fdiv idiv
    std::vector<Timeline> _latches;
    Port _port;
    // VMU 状态
    Cycles _vmuStallUntil = 0;
    double _portCredit = 0;          // 分数占用的累计
    unsigned _outstanding = 0;
    bool _portBlocked = false;
    mem::Packet *_pendingPkt = nullptr;
    BeatState *_pendingBs = nullptr;
    std::deque<uint64_t> _tlb;
    unsigned _vsdqUsed = 0;          // 已读出但尚未发送的 store beat 数
    std::deque<VldqEntry> _vldq;
    std::vector<std::deque<VldqEntry>> _bwq;           // 每 bank
    std::map<std::string, uint64_t> &_vmuCycle;
};

// ------------------------------------------------------------------- VRU
class VRU {
public:
    VRU(Hwacha &h);
    void enqueue(const std::vector<std::string> &cmd);
    void ackBlock();
    void step(Cycles now);
    bool recvTimingResp(mem::Packet *pkt);
    void recvReqRetry() { _blocked = false; }
    mem::RequestPort &port() { return _port; }
    bool idle() const { return _cmdq.empty() && _pfQueue.empty() && !_decoding && _outstanding == 0; }
    std::map<std::string, uint64_t> stats;
private:
    struct Port : mem::RequestPort {
        VRU &v;
        Port(std::string n, VRU &o, sim::SimObject *owner) : RequestPort(std::move(n), owner), v(o) {}
        bool recvTimingResp(mem::Packet *pkt) override { return v.recvTimingResp(pkt); }
        void recvReqRetry() override { v.recvReqRetry(); }
    };
    void prefetchFor(const Instr &ins, unsigned vl);
    uint64_t blockBytes(unsigned vl) const;
    Hwacha &_h;
    const HwachaParams &_p;
    Port _port;
    std::map<std::string, uint64_t> _va;
    unsigned _vl = 0;
    std::deque<std::vector<std::string>> _cmdq;
    std::deque<uint64_t> _pfQueue;
    std::deque<uint64_t> _blockBytes;
    std::deque<uint64_t> _seenLines;
    std::set<uint64_t> _seenSet;
    uint64_t _runaheadBytes = 0;
    unsigned _blocksSeen = 0, _outstanding = 0;
    bool _decoding = false, _blocked = false;
    size_t _decodeIdx = 0;
    unsigned _decodeVl = 0;
};

// ---------------------------------------------------------------- Hwacha
class Hwacha : public sim::ClockedObject {
public:
    Hwacha(std::string name, const HwachaParams &p, const Kernel &k, uint64_t n, unsigned lineBytes, const Trace *trace = nullptr);
    void regStats() override;
    void startup() override;
    bool done() const { return _done; }
    Cycles cycles() const { return _endCycle; }
    Lane &lane(int i) { return *_lanes[i]; }
    VRU *vru() { return _vru.get(); }
    const HwachaParams &params() const { return _p; }
    const Kernel &kernel() const { return _k; }
    unsigned lineBytes() const { return _lineBytes; }
    std::string reportText() const;
    std::string reportJson() const;

    // 供 lane/VRU 使用
    void noteFma(unsigned elems) { _fmaElems += elems; }
    void maybeAck(Block *b);
    std::map<std::string, uint64_t> laneCycle, scalarCycle, ctrlCycle, vmuCycle;

private:
    friend class Lane;
    friend class VRU;
    void tick();
    void ctrlStep(Cycles now);
    void scalarStep(Cycles now);
    void retireStep(Cycles now);
    VectorOp *issueVector(const Instr &ins, Cycles now);
    void setupMem(VectorOp &op, const Instr &ins);
    std::vector<LaneOp::Beat> beatsFor(VectorOp &op, LaneOp &lo, unsigned k);
    void ctrlNextIter();
    void checkKernel();

    HwachaParams _p;
    Kernel _k;
    const Trace *_trace;
    size_t _traceBlock = 0, _tracePos = 0;
    const TraceBlock *_curTrace = nullptr;
    uint64_t _n;
    unsigned _lineBytes;
    unsigned _maxvl;
    std::vector<std::unique_ptr<Lane>> _lanes;
    std::unique_ptr<VRU> _vru;
    sim::EventFunctionWrapper _tickEvent;
    // 主序列器
    std::vector<VectorOp *> _inflight;
    std::vector<std::unique_ptr<VectorOp>> _allOps;
    unsigned _slotsUsed = 0;
    uint64_t _nextOpId = 0;
    std::vector<std::unique_ptr<Block>> _blocks;
    // 命令队列与标量单元
    std::deque<std::vector<std::string>> _vcmdq;
    std::map<std::string, uint64_t> _va;
    unsigned _vl = 0;
    bool _vfActive = false;
    size_t _pc = 0;
    Block *_curBlock = nullptr;
    Cycles _stallUntil = 0;
    struct SbEntry { Cycles ready; VectorOp *op; };
    std::map<std::string, SbEntry> _scoreboard;
    VectorOp *_pendingBranch = nullptr;
    const Instr *_pendingBranchIns = nullptr;
    std::map<int, int> _branchTaken;
    // 控制线程
    unsigned _ctrlIter = 0, _itersTotal = 0;
    uint64_t _ctrlRemaining;
    std::deque<std::vector<std::string>> _ctrlCmds;
    Cycles _ctrlBusyUntil = 0;
    bool _ctrlDone = false;
    unsigned _ctrlCycles;
    std::map<std::string, uint64_t> _vaOffsets;
    // 结束与统计
    bool _done = false;
    Cycles _endCycle = 0;
    uint64_t _fmaElems = 0, _instrsIssued = 0, _vectorOps = 0, _occSum = 0, _lastProgress = 0;
    Cycles _lastProgressCycle = 0;
    std::vector<std::string> _warnings;
};

}  // namespace hw
