#include "hwacha.hh"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hw {
using sim::curTick;

// --------------------------------------------------------------- 参数
// VSDQ 条目：单位步长每个 16 B beat 一条；跨步/索引 store 每元素一个请求，但数据量按元素大小折算成 16 B 条目
static unsigned vsdqEntriesFor(const Instr &ins, size_t nbeats, unsigned tb) {
    if (nbeats == 0 || ins.mode == Mode::Unit) return (unsigned)nbeats;   // 全被谓词屏蔽的 strip 不占条目
    return std::max<unsigned>(1, (unsigned)((nbeats * ins.elsize + tb - 1) / tb));
}

HwachaParams HwachaParams::from(const sim::Params &p) {
    HwachaParams h;
    auto I = [&](const char *k, unsigned &f) { f = (unsigned)p.getInt(k, f); };
    I("n_lanes", h.nLanes); I("n_banks", h.nBanks); I("bank_width", h.bankWidth); I("reg_len", h.regLen);
    I("n_sram_entries", h.nSramEntries); I("n_pred_entries", h.nPredEntries); I("n_vector_regs", h.nVectorRegs);
    I("n_pred_regs", h.nPredRegs); I("n_seq_entries", h.nSeqEntries); I("n_fma_units", h.nFmaUnits);
    h.confPrec = p.getBool("conf_prec", h.confPrec);
    I("stages_alu", h.stagesAlu); I("stages_plu", h.stagesPlu); I("stages_imul", h.stagesIMul); I("stages_dfma", h.stagesDFma);
    I("stages_sfma", h.stagesSFma); I("stages_hfma", h.stagesHFma); I("stages_fconv", h.stagesFConv); I("stages_fcmp", h.stagesFCmp);
    I("fdiv_cycles_per_elem", h.fdivCyclesPerElem); I("fsqrt_cycles_per_elem", h.fsqrtCyclesPerElem); I("idiv_cycles_per_elem", h.idivCyclesPerElem);
    I("cmdq_len", h.cmdqLen); I("vf_fetch_latency", h.vfFetchLatency); I("scalar_smu_latency", h.scalarSmuLatency);
    I("scalar_fpu_latency", h.scalarFpuLatency); I("scalar_muldiv_latency", h.scalarMulDivLatency);
    I("branch_resolve_latency", h.branchResolveLatency); I("ctrl_cycles_per_iter", h.ctrlCyclesPerIter);
    I("branch_strip_cycles", h.branchStripCycles); h.seqAgeRule = p.getBool("seq_age_rule", h.seqAgeRule); h.pluPort = p.getBool("plu_port", h.pluPort); I("plu_occupancy", h.pluOccupancy);
    I("n_vmt_entries", h.nVmtEntries); I("vmu_issue_latency", h.vmuIssueLatency); I("vlu_latency", h.vluLatency);
    I("vsdq_beats", h.vsdqBeats); I("vldq_beats", h.vldqBeats); I("vvaq_entries", h.vvaqEntries);
    I("brq_depth", h.brqDepth); I("bwq_depth", h.bwqDepth); I("tl_data_bytes", h.tlDataBytes);
    h.buildVru = p.getBool("build_vru", h.buildVru);
    I("vru_max_outstanding", h.vruMaxOutstanding); I("vru_early_ignore", h.vruEarlyIgnore);
    h.vruMaxRunaheadBytes = (uint64_t)p.getInt("vru_max_runahead_bytes", (int64_t)h.vruMaxRunaheadBytes);
    I("tlb_entries", h.tlbEntries); I("tlb_miss_latency", h.tlbMissLatency); I("page_bytes", h.pageBytes);
    h.freqGhz = p.getDouble("freq_ghz", h.freqGhz);
    h.storeBeatCycles = p.getDouble("store_beat_cycles", h.storeBeatCycles);
    h.loadBeatCycles = p.getDouble("load_beat_cycles", h.loadBeatCycles);
    I("vf_block_overhead", h.vfBlockOverhead); I("vf_lane_sync_cycles", h.vfLaneSyncCycles); I("lane_max_lead_beats", h.laneMaxLeadBeats); h.sharedLineStoreTurnaround = p.getDouble("shared_line_store_turnaround", h.sharedLineStoreTurnaround); I("pred_port_cycles", h.predPortCycles); I("pred_port_int_cycles", h.predPortIntCycles); I("branch_pred_port_cycles", h.branchPredPortCycles); I("ibox_lane_elem_cycles", h.iboxLaneElemCycles); h.lockstepIndexed = p.getBool("lockstep_indexed", h.lockstepIndexed);
    h.commitLog = p.getBool("commit_log", false);
    return h;
}

unsigned maxVlen(const HwachaParams &p, const VCfg &vc) {
    if (vc.nvv() == 0) return 0;
    unsigned epb;
    if (p.confPrec) epb = (p.nSramEntries * 4) / (4 * vc.v64 + 2 * vc.v32 + vc.v16);
    else epb = p.nSramEntries / vc.nvv();
    if (vc.vp > 0) epb = std::min(epb, p.nPredEntries / vc.vp);
    epb = std::max(epb, 1u);
    unsigned perLane = std::min(epb * p.nStrip(), p.maxVlenPerLane());
    return std::max(perLane, p.nStrip()) * p.nLanes;
}

unsigned rateOf(const HwachaParams &p, char prec) {
    if (!p.confPrec) return 1;
    return prec == 's' ? 2 : prec == 'h' ? 4 : 1;
}

static unsigned stagesFor(const Instr &ins, const HwachaParams &p) {
    switch (ins.kind) {
        case Kind::Alu: case Kind::Cmp: return p.stagesAlu;
        case Kind::Plu: return p.stagesPlu;
        case Kind::IMul: return p.stagesIMul;
        case Kind::Fma: return ins.prec == 's' ? p.stagesSFma : ins.prec == 'h' ? p.stagesHFma : p.stagesDFma;
        case Kind::FConv: return p.stagesFConv;
        case Kind::FCmp: return p.stagesFCmp;
        default: return 1;
    }
}

static const std::map<std::string, std::vector<unsigned>> LATCH_GROUPS = {
    {"fma0", {0, 1, 2}}, {"imul", {0, 1}}, {"fconv", {2}}, {"fma1", {3, 4, 5}}, {"vqu", {3, 4}}, {"fcmp", {3, 4}}, {"vgu", {5}}, {"plu", {}}};

// --------------------------------------------------------------- LaneOp
unsigned LaneOp::nElems(unsigned k) const {
    unsigned n = 0, r = op->rate;
    for (unsigned i = k * r; i < std::min<unsigned>((k + 1) * r, chunks.size()); ++i) n += chunks[i].second - chunks[i].first;
    return n;
}
unsigned LaneOp::nActive(unsigned k) const {
    if (!op->trace) return nElems(k);
    unsigned n = 0, r = op->rate;
    for (unsigned i = k * r; i < std::min<unsigned>((k + 1) * r, chunks.size()); ++i)
        for (unsigned e = chunks[i].first; e < chunks[i].second; ++e) n += op->activeAt(e);
    return n;
}
std::pair<unsigned, unsigned> LaneOp::stripRange(unsigned k) const {
    unsigned r = op->rate;
    unsigned a = k * r, b = std::min<unsigned>((k + 1) * r, chunks.size()) - 1;
    return {chunks[a].first, chunks[b].second};
}

// ------------------------------------------------------------------ Lane
Lane::Lane(Hwacha &h, int id)
    : _h(h), _p(h.params()), _id(id), _port(h.name() + ".lane" + std::to_string(id) + ".vmu_port", *this, &h),
      _vmuCycle(h.vmuCycle) {
    _bankWrite.resize(_p.nBanks);
    for (const char *u : {"fma0", "fma1", "imul", "fconv", "fcmp", "vqu", "vgu", "fdiv", "idiv", "plu"}) _units.emplace(u, Timeline());
    _latches.resize(6);
    _bwq.resize(_p.nBanks);
}

std::vector<unsigned> Lane::stripsCovering(const LaneOp &a, unsigned e0, unsigned e1) const {
    std::vector<unsigned> out;
    if (a.nstrips == 0) return out;
    unsigned U = _p.nStrip(), nl = _p.nLanes, r = a.op->rate;
    unsigned k0 = ((e0 / U) / nl) / r;
    unsigned k1 = (((e1 - 1) / U) / nl) / r;
    for (unsigned k = k0; k <= std::min(a.nstrips - 1, k1); ++k) out.push_back(k);
    return out;
}

const char *Lane::hazard(LaneOp &b, unsigned k, Cycles now, Cycles writeTime, bool checkWaw) {
    if (b.op->deps.empty()) return nullptr;
    auto [e0, e1] = b.stripRange(k);
    for (auto &d : b.op->deps) {
        LaneOp &a = *d.op->lanes[_id];
        for (unsigned ka : stripsCovering(a, e0, e1)) {
            if (d.raw) { Cycles dn = a.done[ka]; if (dn == NoCycle || dn >= now) return d.op->ins->isMem() ? "raw_mem" : "raw"; }
            if (d.waw && checkWaw && writeTime != NoCycle) { Cycles dn = a.done[ka]; if (dn == NoCycle || dn >= writeTime) return "waw"; }
            if (d.war) { Cycles rt = a.readTime[ka]; if (rt == NoCycle || rt > now) return "war"; }
        }
    }
    return nullptr;
}

bool Lane::loadWriteGate(LaneOp &b, unsigned k, Cycles now) {
    if (b.op->deps.empty()) return true;
    auto [e0, e1] = b.stripRange(k);
    for (auto &d : b.op->deps) {
        if (!(d.waw || d.war)) continue;
        LaneOp &a = *d.op->lanes[_id];
        for (unsigned ka : stripsCovering(a, e0, e1)) {
            if (d.waw && (a.done[ka] == NoCycle || a.done[ka] >= now)) return false;
            if (d.war && (a.readTime[ka] == NoCycle || a.readTime[ka] > now)) return false;
        }
    }
    return true;
}

static bool secondPortKind(const Instr &ins) {
    // RTL 序列器的第二调度端口只服务 VSU / VGU / VQU 类操作（store 数据读出、索引读出、变延迟单元读出）
    switch (ins.kind) {
        case Kind::Store: case Kind::Amo: case Kind::FDiv: case Kind::IDiv: case Kind::RFirst: case Kind::Branch: return true;
        case Kind::Load: return ins.mode == Mode::Indexed;
        default: return false;
    }
}

bool Lane::schedule(Cycles now) {
    std::string reason = ops.empty() ? "empty" : "drain";
    bool issued = false;
    LaneOp *first = nullptr;
    // 三个发射口：0 = 展开器（VXU 算术/访存地址）、1 = VSU/VGU/VQU、2 = VIPU（谓词逻辑 vpop 等有自己的调度器与端口，不参与 age 优先级）
    for (int port = 0; port < 3; ++port) {
        // RTL 序列器（sequencer-lane.scala）：条目发出一个 strip 后 age := nBanks-1 并逐拍递减；每拍先在 age 为 0 的就绪条目里
        // 按年龄找第一个（first_sched），没有才在全部就绪条目里找（second_sched）。于是有多个就绪条目时它们轮流占用发射槽
        // （分支的谓词归约借此与其他指令重叠），只有一个条目时它仍可每拍发射。
        bool got = false;
        // RTL：VSU（store 数据）与 VGU（索引地址）路径只考虑各自最老的条目（sequencer-lane.scala 的 first = ff(active.vsu / vgu)），
        // 否则年轻的 store 先占满 VSDQ 会把 VMU 队头的老 store 卡死
        LaneOp *oldestStore = nullptr, *oldestGather = nullptr;
        for (LaneOp *b : ops) {
            if (b->finished || b->nextStrip >= b->nstrips) continue;
            const Instr &ins = *b->op->ins;
            if (!oldestStore && ins.isStore()) oldestStore = b;
            if (!oldestGather && ins.kind == Kind::Load && ins.mode == Mode::Indexed) oldestGather = b;
        }
        for (int pass = 0; pass < 2 && !got; ++pass) {
            bool strict = _p.seqAgeRule && pass == 0;
            for (LaneOp *b : ops) {
                if (b == first || b->finished || b->nextStrip >= b->nstrips) continue;
                const Instr &ins = *b->op->ins;
                if (ins.isStore() && b != oldestStore) continue;
                if (ins.kind == Kind::Load && ins.mode == Mode::Indexed && b != oldestGather) continue;
                if ((ins.kind == Kind::Load || ins.kind == Kind::PMem) && ins.mode != Mode::Indexed) continue;
                bool plu = ins.kind == Kind::Plu && _p.pluPort;
                if (port == 2 ? !plu : (plu || (port == 1) != secondPortKind(ins))) continue;
                if (strict && !plu && b->lastIssue != NoCycle && now < b->lastIssue + _p.nBanks) continue;
                unsigned k = b->nextStrip;
                const char *r = tryIssue(*b, k, now);
                if (!r) {
                    b->nextStrip++;
                    b->lastIssue = now;
                    b->issue[k] = now;
                    b->readTime[k] = now;
                    ++stripsIssued;
                    issued = true; got = true;
                    first = b;
                    break;
                }
                if (port == 0 && !strict && (reason == "drain" || reason == "empty")) reason = r;
            }
            if (!_p.seqAgeRule) break;
        }
    }
    if (!issued) lastReason = reason;
    return issued;
}

const char *Lane::tryIssue(LaneOp &b, unsigned k, Cycles now) {
    const Instr &ins = *b.op->ins;
    Kind kind = ins.kind;
    std::string skind;
    if (kind == Kind::Amo || (kind == Kind::Load && ins.mode == Mode::Indexed)) skind = "vgu";
    else if (kind == Kind::Store && ins.mode == Mode::Indexed) skind = "vgu_store";
    else skind = kindName(kind);
    unsigned rp = ins.rp;
    unsigned nElems = std::max(1u, b.nActive(k));
    Cycles fop = now + rp + 1;
    bool pred = !ins.pred.empty();
    // 共享谓词端口：谓词化的非 PLU 操作读谓词、vcmp 类（FCmp/Cmp）写谓词都要占 predPortCycles 拍
    // 经验规则（RTL pcmp_* 微基准）：vcmp 类写谓词与浮点类（FMA/FConv/FDiv）谓词化读共用一个端口；整数 ALU/PLU 的谓词读不占
    bool fpClass = kind == Kind::Fma || kind == Kind::FConv || kind == Kind::FDiv;
    bool intClass = kind == Kind::Alu || kind == Kind::IMul || kind == Kind::IDiv;
    unsigned predCyc = ((pred && fpClass) || ((kind == Kind::FCmp || kind == Kind::Cmp) && ins.writesPrf())) ? _p.predPortCycles
                     : (pred && intClass) ? _p.predPortIntCycles
                     : (kind == Kind::Branch) ? _p.branchPredPortCycles : 0;
    if (rp && !_readPort.free(now, rp)) return "rport";
    if (predCyc && !_predRead.free(now, predCyc)) return "pport";
    std::string unit;
    unsigned lat = 0;
    Cycles writeTime = NoCycle;
    unsigned occupancy = _p.nBanks;
    auto latchesFree = [&](const std::string &u) {
        for (unsigned i : LATCH_GROUPS.at(u)) if (!_latches[i].free(now + 1, rp + 1)) return false;
        return true;
    };
    if (skind == "fma" || skind == "imul" || skind == "fconv" || skind == "fcmp") {
        lat = stagesFor(ins, _p);
        std::vector<std::string> cands = skind == "fma" ? (_p.nFmaUnits == 1 ? std::vector<std::string>{"fma0"} : std::vector<std::string>{"fma0", "fma1"})
                                                        : std::vector<std::string>{skind};
        bool fuFree = false;
        for (auto &u : cands) {
            if (_units[u].free(fop, occupancy)) { fuFree = true; if (latchesFree(u)) { unit = u; break; } }
        }
        if (unit.empty()) return fuFree ? "latch" : "fu";
        writeTime = fop + lat;
    } else if (skind == "alu" || skind == "cmp" || skind == "plu") {
        lat = stagesFor(ins, _p);
        writeTime = fop + lat;
        if (skind == "plu" && _p.pluOccupancy) {   // 谓词逻辑单元每 strip 占用 pluOccupancy 拍（校准用）
            if (!_units["plu"].free(fop, _p.pluOccupancy)) return "fu";
            unit = "plu"; occupancy = _p.pluOccupancy;
        }
    } else if (skind == "fdiv" || skind == "idiv" || skind == "rfirst" || skind == "branch") {
        if (skind == "branch") occupancy = std::max(occupancy, _p.branchStripCycles);
        if (!_units["vqu"].free(fop, occupancy)) return "fu";
        if (!latchesFree("vqu")) return "latch";
        unit = "vqu";
        if (skind == "fdiv" || skind == "idiv") {
            Timeline &div = _units[skind];
            if (!div.free(fop)) return "fu";
            unsigned per = skind == "fdiv" ? (ins.mnemonic.rfind("vfsqrt", 0) == 0 ? _p.fsqrtCyclesPerElem : _p.fdivCyclesPerElem) : _p.idivCyclesPerElem;
            writeTime = div.firstFree(fop) + per * nElems + 2;
        } else {
            writeTime = fop + _p.nBanks + (skind == "branch" ? _p.branchResolveLatency : 4);
        }
    } else if (skind == "store") {
        if (_vsdqUsed + vsdqEntriesFor(ins, b.beats[k].size(), _p.tlDataBytes) > _p.vsdqBeats) return "vsdq";
    } else if (skind == "vgu" || skind == "vgu_store") {
        if (!_units["vgu"].free(fop, occupancy)) return "fu";
        if (!latchesFree("vgu")) return "latch";
        unit = "vgu";
        if (skind == "vgu_store" && _vsdqUsed + vsdqEntriesFor(ins, b.beats[k].size(), _p.tlDataBytes) > _p.vsdqBeats) return "vsdq";
    }
    // 写口：展开器写 µop 在 writeTime + bank 号到达各 bank
    bool vrfWrite = writeTime != NoCycle && ins.writesVrf() && skind != "rfirst" && skind != "branch";
    if (writeTime != NoCycle) {
        if (ins.writesPrf()) { if (!_predWrite.free(writeTime)) return "wport"; }
        else if (vrfWrite) { for (unsigned bk = 0; bk < _p.nBanks; ++bk) if (!_bankWrite[bk].free(writeTime + bk)) return "wport"; }
    }
    if (const char *h = hazard(b, k, now, writeTime, true)) return h;
    // ---- 发射 ----
    if (rp) { _readPort.reserve(now, rp); readPortBusy += rp; }
    if (predCyc) _predRead.reserve(now, predCyc);
    if (!unit.empty()) {
        _units[unit].reserve(fop, occupancy);
        for (unsigned i : LATCH_GROUPS.at(unit)) _latches[i].reserve(now + 1, rp + 1);
        std::string key = unit.rfind("fma", 0) == 0 ? "fma" : unit;
        fuBusy[key] += occupancy;
        if (skind == "fdiv" || skind == "idiv") {
            Timeline &d = _units[skind];
            Cycles s0 = d.firstFree(fop);
            d.reserve(s0, unsigned(writeTime - 2 - s0));
            fuBusy[skind] += writeTime - 2 - s0;
        }
    }
    if (skind == "fma") _h.noteFma(b.nActive(k));
    if (writeTime != NoCycle) {
        if (ins.writesPrf()) _predWrite.reserve(writeTime);
        else if (vrfWrite) { for (unsigned bk = 0; bk < _p.nBanks; ++bk) _bankWrite[bk].reserve(writeTime + bk); ++writePortBusy; }
        b.done[k] = writeTime;
    }
    if (skind == "store") { b.dataReady[k] = now + 2; b.vsdqEntries[k] = vsdqEntriesFor(ins, b.beats[k].size(), _p.tlDataBytes); _vsdqUsed += b.vsdqEntries[k]; }
    else if (skind == "vgu_store") { b.dataReady[k] = fop + 1; b.addrReady[k] = fop + 1; b.vsdqEntries[k] = vsdqEntriesFor(ins, b.beats[k].size(), _p.tlDataBytes); _vsdqUsed += b.vsdqEntries[k]; }
    else if (skind == "vgu") { b.addrReady[k] = fop + 1; b.dataReady[k] = fop + 1; }
    return nullptr;
}

// ---- VMU：每周期最多发一个 beat ----
void Lane::vmuStep(Cycles now) {
    vmuReason = "idle";
    if (_portBlocked) { vmuReason = "port_busy"; return; }
    // 重试路径本拍已经发过一个 beat：VMU 每拍只有一个 A 通道请求（否则重试的 lane 会在一拍内发两个 beat，永远领先另一条 lane 一行）
    if (_lastBeatCycle == now) { vmuReason = "busy"; return; }
    if (now < _vmuStallUntil) { vmuReason = "tlb"; return; }
    if (_portCredit >= 1.0) { _portCredit -= 1.0; vmuReason = "port_occ"; return; }
    if (vmuQueue.empty()) return;
    LaneOp *b = vmuQueue.front();
    if (b->nstrips == 0) { vmuQueue.pop_front(); return; }
    if (now < b->vmuStart) { vmuReason = "issue_lat"; return; }
    unsigned s = b->stripPtr;
    const Instr &ins = *b->op->ins;
    bool isStore = ins.isStore();
    if (isStore || ins.mode == Mode::Indexed) {
        Cycles need = isStore ? b->dataReady[s] : b->addrReady[s];
        if (need == NoCycle || need > now) { vmuReason = isStore ? "wait_data" : "wait_addr"; return; }
    }
    if (_outstanding >= _p.nVmtEntries) { vmuReason = "vmt_full"; return; }
    // lane 间锁步：RTL 的各 lane 在同一条访存指令上几乎同步推进（2 bank L2 下 32 位内核两条 lane 撞同一个 bank，
    // 只快 16%–23%，见 docs/24 §10.13）；限制本 lane 不能比最慢的 lane 多发超过 laneMaxLeadBeats 个 beat
    if (_p.laneMaxLeadBeats && _p.nLanes > 1 && (_p.lockstepIndexed || ins.mode != Mode::Indexed)) {
        // 只与还有 beat 要发的 lane 比较（谓词化访存各 lane 的 beat 数不同，已发完的 lane 不再约束别人）
        uint64_t slowest = b->beatsSent;
        for (auto &other : b->op->lanes)
            if (other && other.get() != b && other->nstrips && other->stripPtr < other->nstrips) slowest = std::min(slowest, other->beatsSent);
        if (b->beatsSent >= slowest + _p.laneMaxLeadBeats) { vmuReason = "lane_sync"; return; }   // 发出这个 beat 后领先不得超过 laneMaxLeadBeats
    }
    // VCU 序列器操作的语义：load 的 WAW/WAR 冒险未清除前不放行该 strip 的请求，
    // 否则返回数据会在 VLDQ 中队头阻塞（硬件亦由序列器在 VCU 处检查）
    if (ins.isLoad() && b->beatPtr == 0 && !loadWriteGate(*b, s, now)) { vmuReason = "wait_hazard"; return; }
    if (b->beats[s].empty()) {   // 该 strip 的 beat 已被前一 strip 覆盖
        b->stripPtr++; b->beatPtr = 0; b->stripsSent++;
        if (b->stripPtr >= b->nstrips) vmuQueue.pop_front();
        vmuReason = "busy";
        return;
    }
    const LaneOp::Beat &beat = b->beats[s][b->beatPtr];
    // 多 lane 时一个 strip 不足一行（32 位元素：8 个元素 = 32 B），两条 lane 各写半行，L2 要把两半合并成一次读-改-写；
    // RTL（probe7 2 bank）显示这样的行每行多花约 1 拍且不能被另一个 bank 掩盖——按 lane 侧换行停顿建模
    if (_p.sharedLineStoreTurnaround > 0 && _p.nLanes > 1 && _p.l2Banks > 1 && isStore && ins.mode == Mode::Unit && ins.elsize * _p.nStrip() < _h.lineBytes()) {
        uint64_t line = beat.addr / _h.lineBytes();
        if (b->lastLine != ~0ull && line != b->lastLine) {
            b->lastLine = line;
            _portCredit += _p.sharedLineStoreTurnaround;
            if (_portCredit >= 1.0) { _portCredit -= 1.0; vmuReason = "line_turn"; return; }
        }
        b->lastLine = line;
    }
    uint64_t page = beat.addr / _p.pageBytes;
    auto it = std::find(_tlb.begin(), _tlb.end(), page);
    if (it == _tlb.end()) {
        if (_tlb.size() >= _p.tlbEntries) _tlb.pop_front();
        _tlb.push_back(page);
        _vmuStallUntil = now + _p.tlbMissLatency;
        vmuReason = "tlb";
        return;
    }
    _tlb.erase(it); _tlb.push_back(page);
    mem::Packet::Cmd cmd = ins.kind == Kind::Amo ? mem::Packet::AtomicReq : isStore ? mem::Packet::WriteReq : mem::Packet::ReadReq;
    if (_p.iboxLaneElemCycles > 1 && _p.nLanes > 1 && ins.mode == Mode::Indexed && _lastIndexedCycle != NoCycle && now < _lastIndexedCycle + _p.iboxLaneElemCycles) { vmuReason = "ibox"; return; }
    // 单位步长每 beat 16 B；跨步/索引访存每元素一个请求，大小为元素大小（L2 据此区分部分写）
    auto *pkt = new mem::Packet(cmd, beat.addr, ins.mode == Mode::Unit ? _p.tlDataBytes : ins.elsize);
    auto *bs = new BeatState(); bs->lo = b; bs->strip = s; bs->beat = b->beatPtr;
    pkt->pushSenderState(bs);
    if (!_port.sendTimingReq(pkt)) { _portBlocked = true; _pendingPkt = pkt; _pendingBs = bs; vmuReason = "port_busy"; return; }
    _lastBeatCycle = now;
    if (ins.mode == Mode::Indexed) _lastIndexedCycle = now;
    ++_outstanding; ++b->beatsSent;
    if (ins.isLoad()) ++loadBeats; else ++storeBeats;
    // store_beat_cycles 只作用于单位步长的 16 B store beat（RTL：跨步/索引 store 与 load 一样每元素 1 拍）
    _portCredit += ((isStore && ins.mode == Mode::Unit) ? _p.storeBeatCycles : _p.loadBeatCycles) - 1.0;
    vmuReason = "busy";
    if (++b->beatPtr >= b->beats[s].size()) {
        if (isStore) _vsdqUsed -= std::min(_vsdqUsed, b->vsdqEntries[s]);   // strip 的数据全部发出，释放其 VSDQ 条目
        b->beatPtr = 0; b->stripPtr++; b->stripsSent++;
        if (b->stripPtr >= b->nstrips) vmuQueue.pop_front();
    }
}

void Lane::recvReqRetry() {
    if (!_portBlocked || !_pendingPkt) return;
    mem::Packet *pkt = _pendingPkt;
    BeatState *bs = _pendingBs;
    if (_port.sendTimingReq(pkt)) {
        _pendingPkt = nullptr; _pendingBs = nullptr; _portBlocked = false;
        LaneOp *b = bs->lo;
        const Instr &ins = *b->op->ins;
        _lastBeatCycle = _h.curCycle();
        if (ins.mode == Mode::Indexed) _lastIndexedCycle = _h.curCycle();
        ++_outstanding; ++b->beatsSent;
        if (ins.isLoad()) ++loadBeats; else ++storeBeats;
        if (++b->beatPtr >= b->beats[bs->strip].size()) {
            if (ins.isStore()) _vsdqUsed -= std::min(_vsdqUsed, b->vsdqEntries[bs->strip]);
            b->beatPtr = 0; b->stripPtr++; b->stripsSent++;
            if (b->stripPtr >= b->nstrips) vmuQueue.pop_front();
        }
    }
}

bool Lane::recvTimingResp(mem::Packet *pkt) {
    auto *bs = static_cast<BeatState *>(pkt->senderStates.back());
    LaneOp *b = bs->lo;
    bool isLoad = pkt->cmd == mem::Packet::ReadResp || pkt->cmd == mem::Packet::AtomicResp;
    if (isLoad) {
        if (_vldq.size() >= _p.vldqBeats) return false;      // VLDQ 满：反压到交叉开关
        _vldq.push_back(VldqEntry{b, bs->strip, bs->beat, _h.curCycle()});
    }
    b->beatsReturned[bs->strip]++;
    ++beatsReturned;
    --_outstanding;
    pkt->popSenderState(); delete bs; delete pkt;
    return true;
}

// ---- VLU：VLDQ → 每 bank BWQ → bank 写口（展开器写优先） ----
void Lane::vluStep(Cycles now) {
    // 1) bank 写口仲裁：每 bank 每周期一个 BWQ 写
    for (unsigned bk = 0; bk < _p.nBanks; ++bk) {
        auto &q = _bwq[bk];
        if (q.empty() || !_bankWrite[bk].free(now)) continue;
        VldqEntry e = q.front(); q.pop_front();
        _bankWrite[bk].reserve(now);
        ++writePortBusy;
        LaneOp *b = e.lo;
        // 一个 beat 可能跨多个 bank：最后一个 bank 写完才算写回
        auto &beat = b->beats[e.strip][e.beat];
        if (bk == beat.banks.back()) {
            b->beatsWritten[e.strip]++;
            b->lastWrite[e.strip] = std::max(b->lastWrite[e.strip], now);
        }
    }
    // 2) VLDQ → BWQ（每周期一个 beat）
    if (_vldq.empty()) return;
    VldqEntry &e = _vldq.front();
    if (e.arrived + _p.vluLatency > now) return;
    LaneOp *b = e.lo;
    if (!loadWriteGate(*b, e.strip, now)) return;
    auto &beat = b->beats[e.strip][e.beat];
    for (unsigned bk : beat.banks) if (_bwq[bk].size() >= _p.bwqDepth) return;
    bool wasFull = _vldq.size() >= _p.vldqBeats;
    for (unsigned bk : beat.banks) _bwq[bk].push_back(e);
    _vldq.pop_front();
    if (wasFull) _port.sendRespRetry();
}

void Lane::memCompletionStep(Cycles now) {
    for (LaneOp *b : ops) {
        const Instr &ins = *b->op->ins;
        if (b->finished || !ins.isMem()) continue;
        while (b->nextDone < b->nstrips) {
            unsigned k = b->nextDone;
            if (k >= b->stripsSent) break;
            unsigned nb = b->beats[k].size();
            if (ins.isLoad()) {
                if (b->beatsWritten[k] < nb) break;
                if (nb && b->lastWrite[k] >= now) break;
                b->done[k] = nb ? b->lastWrite[k] : now;
            } else {
                if (b->beatsReturned[k] < nb) break;
                b->done[k] = now;
            }
            b->readTime[k] = b->issue[k] != NoCycle ? b->issue[k] : now;
            b->nextDone++;
        }
        if (b->nextDone >= b->nstrips) {
            b->finished = true;
            Cycles m = 0;
            for (Cycles d : b->done) if (d != NoCycle) m = std::max(m, d);
            b->finishCycle = m;
        }
    }
}

void Lane::retireStep(Cycles now) {
    for (LaneOp *b : ops) {
        if (b->finished || b->op->ins->isMem()) continue;
        if (b->nextStrip >= b->nstrips) {
            Cycles m = 0; bool all = true;
            for (Cycles d : b->done) { if (d == NoCycle) { all = false; break; } m = std::max(m, d); }
            if (all && now >= m) { b->finished = true; b->finishCycle = m; }
        }
    }
    ops.erase(std::remove_if(ops.begin(), ops.end(), [](LaneOp *b) { return b->finished && b->op->complete; }), ops.end());
}

// ------------------------------------------------------------------- VRU
VRU::VRU(Hwacha &h) : _h(h), _p(h.params()), _port(h.name() + ".vru_port", *this, &h) {}

void VRU::enqueue(const std::vector<std::string> &cmd) { if (_cmdq.size() < _p.cmdqLen) _cmdq.push_back(cmd); }
void VRU::ackBlock() { if (!_blockBytes.empty()) { _runaheadBytes -= _blockBytes.front(); _blockBytes.pop_front(); } }

bool VRU::recvTimingResp(mem::Packet *pkt) { if (_outstanding) --_outstanding; delete pkt; return true; }

uint64_t VRU::blockBytes(unsigned vl) const {
    uint64_t n = 0;
    for (auto &ins : _h.kernel().instrs)
        if (ins.isMem() && (ins.mode == Mode::Unit || ins.mode == Mode::Stride)) n += uint64_t(vl) * ins.elsize * (ins.seglen + 1);
    return n;
}

void VRU::prefetchFor(const Instr &ins, unsigned vl) {
    auto [baseReg, strideReg] = ins.memRegs();
    auto it = _va.find(baseReg);
    if (it == _va.end()) return;
    uint64_t base = it->second, lb = _h.lineBytes();
    std::vector<uint64_t> lines;
    if (ins.mode == Mode::Unit) {
        uint64_t step = ins.elsize * (ins.seglen + 1);
        for (uint64_t ln = base / lb; ln < (base + vl * step + lb - 1) / lb; ++ln) lines.push_back(ln);
    } else {
        uint64_t stride = strideReg.empty() ? ins.elsize : (_va.count(strideReg) ? _va[strideReg] : ins.elsize);
        uint64_t last = ~0ull;
        for (unsigned e = 0; e < vl; ++e) { uint64_t ln = (base + e * stride) / lb; if (ln != last) { lines.push_back(ln); last = ln; } }
    }
    for (uint64_t ln : lines) {
        if (_seenSet.count(ln)) continue;
        _seenSet.insert(ln); _seenLines.push_back(ln);
        if (_seenLines.size() > 4096) { _seenSet.erase(_seenLines.front()); _seenLines.pop_front(); }
        _pfQueue.push_back(ln);
    }
}

void VRU::step(Cycles now) {
    if (!_pfQueue.empty() && !_blocked) {
        if (_outstanding < _p.vruMaxOutstanding) {
            uint64_t ln = _pfQueue.front();
            auto *pkt = new mem::Packet(mem::Packet::PrefetchReq, ln * _h.lineBytes(), _h.lineBytes());
            if (_port.sendTimingReq(pkt)) { _pfQueue.pop_front(); ++_outstanding; ++stats["prefetch_lines"]; }
            else { delete pkt; _blocked = true; }
        } else ++stats["outstanding_full_cycles"];
    }
    if (_decoding) {
        const Instr &ins = _h.kernel().instrs[_decodeIdx];
        if (ins.kind == Kind::Stop) { _decoding = false; return; }
        if (ins.isMem() && (ins.mode == Mode::Unit || ins.mode == Mode::Stride)) prefetchFor(ins, _decodeVl);
        ++_decodeIdx;
        return;
    }
    if (_runaheadBytes > _p.vruMaxRunaheadBytes) { ++stats["throttle_cycles"]; return; }
    if (_cmdq.empty()) return;
    auto cmd = _cmdq.front(); _cmdq.pop_front();
    if (cmd[0] == "vsetvl") _vl = std::stoul(cmd[1]);
    else if (cmd[0] == "vmca") _va[cmd[1]] = std::stoull(cmd[2]);
    else if (cmd[0] == "vf") {
        ++_blocksSeen;
        uint64_t nb = blockBytes(_vl);
        _blockBytes.push_back(nb); _runaheadBytes += nb;
        if (_blocksSeen <= _p.vruEarlyIgnore) { ++stats["blocks_skipped"]; return; }
        ++stats["blocks_decoded"];
        _decoding = true; _decodeIdx = 0; _decodeVl = _vl;
    }
}

// ---------------------------------------------------------------- Hwacha
Hwacha::Hwacha(std::string n, const HwachaParams &p, const Kernel &k, uint64_t nElems, unsigned lineBytes, const Trace *trace)
    : ClockedObject(std::move(n), p.period()), _p(p), _k(k), _trace(trace), _n(nElems), _lineBytes(lineBytes),
      _tickEvent([this] { tick(); }, name() + ".tick", sim::Event::CPU_Tick_Pri), _ctrlRemaining(nElems) {
    _maxvl = maxVlen(_p, _k.vcfg);
    for (unsigned i = 0; i < _p.nLanes; ++i) _lanes.emplace_back(new Lane(*this, (int)i));
    if (_p.buildVru) _vru.reset(new VRU(*this));
    _ctrlCycles = _k.ctrlCycles >= 0 ? (unsigned)_k.ctrlCycles : _p.ctrlCyclesPerIter;
    _itersTotal = _trace ? (unsigned)_trace->blocks.size() : _k.iters ? (unsigned)_k.iters : (unsigned)((_n + _maxvl - 1) / _maxvl);
    for (auto &r : _k.vaOrder) _vaOffsets[r] = 0;
    _ctrlCmds.push_back({"vsetcfg"});
    ctrlNextIter();
    checkKernel();
}

void Hwacha::checkKernel() {
    for (auto &ins : _k.instrs) {
        for (auto &r : ins.reads()) if (r.rfind("vv", 0) == 0 && (unsigned)regIndex(r) >= _k.vcfg.nvv()) _warnings.push_back(ins.text + ": " + r + " exceeds vsetcfg");
        for (auto &r : ins.writes()) if (r.rfind("vv", 0) == 0 && (unsigned)regIndex(r) >= _k.vcfg.nvv()) _warnings.push_back(ins.text + ": " + r + " exceeds vsetcfg");
        for (auto &r : ins.reads()) if (r.rfind("vp", 0) == 0 && (unsigned)regIndex(r) >= _k.vcfg.vp) _warnings.push_back(ins.text + ": " + r + " exceeds vsetcfg");
    }
}

void Hwacha::regStats() {}
void Hwacha::startup() { sim::mainEventQueue().schedule(&_tickEvent, clockEdge(1)); }

// ---- 控制线程 ----
void Hwacha::ctrlNextIter() {
    if (_trace) {
        // 执行驱动：每个踪迹块对应一次 vf，vl 取踪迹值
        if (_ctrlIter >= _trace->blocks.size()) { _ctrlDone = true; return; }
    } else if (_ctrlIter >= _itersTotal || _ctrlRemaining == 0) { _ctrlDone = true; return; }
    unsigned vl = _trace ? _trace->blocks[_ctrlIter].vl : (unsigned)std::min<uint64_t>(_ctrlRemaining, _maxvl);
    _ctrlCmds.push_back({"vsetvl", std::to_string(vl)});
    for (auto &reg : _k.vaOrder) {
        const VaDef &d = _k.va.at(reg);
        uint64_t addr;
        if (d.isPtr) {
            const ArrayDef &arr = _k.arrays.at(d.array);
            addr = arr.base + uint64_t(d.offsetElems) * arr.elem + _vaOffsets[reg];
            if (d.advance) _vaOffsets[reg] = (_vaOffsets[reg] + uint64_t(vl) * arr.elem * d.strideElems) % std::max<uint64_t>(arr.elem, arr.nbytes());
        } else addr = d.value;
        _ctrlCmds.push_back({"vmca", reg, std::to_string(addr)});
    }
    for (unsigned i = 0; i < _k.nVmcs; ++i) _ctrlCmds.push_back({"vmcs"});
    _ctrlCmds.push_back({"vf", std::to_string(vl)});
    _ctrlCmds.push_back({"_ctrl", std::to_string(_ctrlCycles)});
    _ctrlRemaining -= std::min<uint64_t>(_ctrlRemaining, vl);
    ++_ctrlIter;
}

void Hwacha::ctrlStep(Cycles now) {
    if (now < _ctrlBusyUntil) { ++ctrlCycle["bookkeeping"]; return; }
    if (_ctrlCmds.empty()) {
        if (_ctrlDone) { ++ctrlCycle["done"]; return; }
        ctrlNextIter();
        if (_ctrlCmds.empty()) return;
    }
    auto &cmd = _ctrlCmds.front();
    if (cmd[0] == "_ctrl") { _ctrlBusyUntil = now + std::stoul(cmd[1]); _ctrlCmds.pop_front(); ++ctrlCycle["bookkeeping"]; return; }
    if (_vcmdq.size() >= _p.cmdqLen) { ++ctrlCycle["vcmdq_full"]; return; }
    _vcmdq.push_back(cmd);
    if (_vru && cmd[0] != "vmcs") _vru->enqueue(cmd);
    _ctrlCmds.pop_front();
    ++ctrlCycle["issue"];
}

// ---- 标量单元 ----
void Hwacha::maybeAck(Block *b) {
    if (b->stopped && b->pendingOps == 0 && !b->acked) { b->acked = true; if (_vru) _vru->ackBlock(); }
}

void Hwacha::scalarStep(Cycles now) {
    auto note = [&](const char *r) { ++scalarCycle[r]; };
    if (now < _stallUntil) { note("latency"); return; }
    if (_pendingBranch) {
        if (!_pendingBranch->complete || now < _pendingBranch->completeCycle) { note("branch"); return; }
        const Instr &ins = *_pendingBranchIns;
        _pendingBranch = nullptr;
        if (_curTrace) { ++_tracePos; }
        else {
            int budget = ins.annot.count("taken") ? std::stoi(ins.annot.at("taken")) : 0;
            int &cnt = _branchTaken[ins.lineNo];
            if (cnt < budget) { ++cnt; _pc = _k.labels.at(ins.label); } else ++_pc;
        }
        note("issue");
        return;
    }
    if (!_vfActive) {
        if (_vcmdq.empty()) { note("idle"); return; }
        auto cmd = _vcmdq.front(); _vcmdq.pop_front();
        if (cmd[0] == "vsetvl") _vl = std::stoul(cmd[1]);
        else if (cmd[0] == "vmca") _va[cmd[1]] = std::stoull(cmd[2]);
        else if (cmd[0] == "vf") {
            _vfActive = true; _pc = 0; _branchTaken.clear();
            _curTrace = nullptr; _tracePos = 0;
            if (_trace) {
                if (_traceBlock < _trace->blocks.size()) {
                    _curTrace = &_trace->blocks[_traceBlock++];
                    if (_curTrace->vl != _vl) { _warnings.push_back("trace vl " + std::to_string(_curTrace->vl) + " != model vl " + std::to_string(_vl)); _vl = _curTrace->vl; }
                } else if (_warnings.empty() || _warnings.back().rfind("trace exhausted", 0) != 0) _warnings.push_back("trace exhausted; falling back to static block");
            }
            _blocks.emplace_back(new Block{(int)_blocks.size(), _vl});
            _curBlock = _blocks.back().get(); _curBlock->start = now;
            _stallUntil = now + _p.vfFetchLatency + _p.vfBlockOverhead + (_p.nLanes > 1 ? _p.vfLaneSyncCycles : 0);
        }
        note("command");
        return;
    }
    const TraceInstr *ti = nullptr;
    if (_curTrace) {
        if (_tracePos >= _curTrace->instrs.size()) { _vfActive = false; _curBlock->stopped = true; maybeAck(_curBlock); note("issue"); return; }
        ti = &_curTrace->instrs[_tracePos];
        uint64_t idx = (ti->pc - _curTrace->basePc) / 8;
        if (ti->pc < _curTrace->basePc || idx >= _k.instrs.size()) sim::fatal("trace pc outside kernel block (index " + std::to_string(idx) + "); use --trace-base");
        _pc = (size_t)idx;
    }
    const Instr &ins = _k.instrs[_pc];
    for (auto &r : ins.srcs) {
        if (r.rfind("vs", 0) == 0) {
            auto it = _scoreboard.find(r);
            if (it != _scoreboard.end()) {
                Cycles ready = it->second.op ? (it->second.op->complete ? it->second.op->completeCycle : NoCycle) : it->second.ready;
                if (ready == NoCycle || ready > now) { note("scoreboard"); return; }
                _scoreboard.erase(it);
            }
        }
    }
    switch (ins.kind) {
        case Kind::Stop:
            _vfActive = false; _curBlock->stopped = true; maybeAck(_curBlock);
            note("issue"); ++_instrsIssued; return;
        case Kind::Fence: {
            bool pend = std::any_of(_inflight.begin(), _inflight.end(), [](VectorOp *o) { return o->ins->isMem() && !o->complete; });
            if (pend) { note("fence"); return; }
            ++_pc; ++_tracePos; note("issue"); return;
        }
        case Kind::Scalar: ++_pc; ++_tracePos; note("issue"); ++_instrsIssued; return;
        case Kind::SLoad: case Kind::SStore: case Kind::SFp: case Kind::SMulDiv: {
            unsigned lat = ins.kind == Kind::SLoad ? _p.scalarSmuLatency : ins.kind == Kind::SStore ? 1 : ins.kind == Kind::SFp ? _p.scalarFpuLatency : _p.scalarMulDivLatency;
            if (ins.dst.rfind("vs", 0) == 0) _scoreboard[ins.dst] = SbEntry{now + lat, nullptr};
            ++_pc; ++_tracePos; note("issue"); ++_instrsIssued; return;
        }
        default: break;
    }
    if (_slotsUsed + ins.slots > _p.nSeqEntries) { note("seq_full"); return; }
    VectorOp *op = issueVector(ins, now);
    op->trace = ti;
    if (ins.isMem() && ti) {
        // 用踪迹重算访存 beat（活跃掩码与索引地址）
        for (unsigned l = 0; l < _p.nLanes; ++l) {
            LaneOp *lo = op->lanes[l].get();
            lo->beats.clear();
            uint64_t lastAddr = ~0ull;
            for (unsigned k = 0; k < lo->nstrips; ++k) {
                auto bs = beatsFor(*op, *lo, k);
                if (!bs.empty() && bs.front().addr == lastAddr) bs.erase(bs.begin());
                if (!bs.empty()) lastAddr = bs.back().addr;
                lo->beats.push_back(std::move(bs));
            }
        }
    }
    ++_instrsIssued; ++_vectorOps;
    if (ins.kind == Kind::Branch) { _pendingBranch = op; _pendingBranchIns = &ins; }
    else if (ins.kind == Kind::RFirst && !ins.dst.empty()) _scoreboard[ins.dst] = SbEntry{NoCycle, op};
    if (ins.kind != Kind::Branch) { ++_pc; ++_tracePos; }
    note("issue");
}

// ---- 向量发射 ----
VectorOp *Hwacha::issueVector(const Instr &ins, Cycles now) {
    unsigned rate = 1;
    if (_p.confPrec) {
        std::vector<unsigned> rs;
        for (auto &r : ins.reads()) if (r.rfind("vv", 0) == 0) rs.push_back(rateOf(_p, _k.vcfg.region(regIndex(r))));
        for (auto &r : ins.writes()) if (r.rfind("vv", 0) == 0) rs.push_back(rateOf(_p, _k.vcfg.region(regIndex(r))));
        if (!rs.empty()) rate = *std::min_element(rs.begin(), rs.end());
        else rate = rateOf(_p, ins.prec);
    }
    unsigned U = _p.nStrip(), E = U * rate, vl = _vl;
    _allOps.emplace_back(new VectorOp());
    VectorOp *op = _allOps.back().get();
    op->id = _nextOpId++; op->ins = &ins; op->block = _curBlock; op->issueCycle = now;
    op->vl = vl; op->rate = rate; op->E = E; op->slots = ins.slots;
    op->reads = ins.reads(); op->writes = ins.writes();
    for (VectorOp *A : _inflight) {
        if (A->complete) continue;
        VectorOp::Dep d{A, false, false, false};
        for (auto &r : op->reads) if (A->writes.count(r)) { d.raw = true; break; }
        for (auto &w : op->writes) { if (A->reads.count(w)) d.war = true; if (A->writes.count(w)) d.waw = true; }
        if (d.raw || d.war || d.waw) op->deps.push_back(d);
    }
    unsigned totalChunks = (vl + U - 1) / U;
    std::vector<std::vector<std::pair<unsigned, unsigned>>> perLane(_p.nLanes);
    for (unsigned c = 0; c < totalChunks; ++c) perLane[c % _p.nLanes].push_back({c * U, std::min((c + 1) * U, vl)});
    if (ins.isMem()) setupMem(*op, ins);
    for (unsigned l = 0; l < _p.nLanes; ++l) {
        auto *lo = new LaneOp();
        lo->op = op; lo->lane = (int)l; lo->chunks = perLane[l];
        lo->nstrips = lo->chunks.empty() ? 0 : (unsigned)((lo->chunks.size() + rate - 1) / rate);
        lo->issue.assign(lo->nstrips, NoCycle); lo->readTime.assign(lo->nstrips, NoCycle); lo->done.assign(lo->nstrips, NoCycle);
        lo->dataReady.assign(lo->nstrips, NoCycle); lo->addrReady.assign(lo->nstrips, NoCycle); lo->vsdqEntries.assign(lo->nstrips, 0);
        lo->finished = lo->nstrips == 0; if (lo->finished) lo->finishCycle = now;
        if (ins.isMem()) {
            lo->vmuStart = now + _p.vmuIssueLatency;
            lo->beatsReturned.assign(lo->nstrips, 0); lo->beatsWritten.assign(lo->nstrips, 0); lo->lastWrite.assign(lo->nstrips, 0);
            uint64_t lastAddr = ~0ull;
            for (unsigned k = 0; k < lo->nstrips; ++k) {
                auto bs = beatsFor(*op, *lo, k);
                if (!bs.empty() && bs.front().addr == lastAddr) bs.erase(bs.begin());   // 与上一 strip 共享的 beat
                if (!bs.empty()) lastAddr = bs.back().addr;
                lo->beats.push_back(std::move(bs));
            }
            if (lo->nstrips) _lanes[l]->vmuQueue.push_back(lo);
        }
        op->lanes.emplace_back(lo);
        _lanes[l]->ops.push_back(lo);
    }
    _inflight.push_back(op);
    _slotsUsed += op->slots;
    _curBlock->pendingOps++;
    return op;
}

void Hwacha::setupMem(VectorOp &op, const Instr &ins) {
    auto [baseReg, strideReg] = ins.memRegs();
    if ((ins.mode == Mode::Unit || ins.mode == Mode::Stride) && baseReg.rfind("va", 0) == 0) {
        op.base = _va.count(baseReg) ? _va[baseReg] : 0;
        op.strideBytes = !strideReg.empty() ? (_va.count(strideReg) ? _va[strideReg] : ins.elsize) : uint64_t(ins.elsize) * (ins.seglen + 1);
    } else if (ins.mode == Mode::Unit || ins.mode == Mode::Stride) {
        op.base = 0x90000000ull; op.strideBytes = uint64_t(ins.elsize) * (ins.seglen + 1);
    } else {
        auto it = ins.annot.find("gather");
        if (it != ins.annot.end()) {
            std::istringstream is(it->second); std::string arr, pat; is >> arr >> pat;
            if (_k.arrays.count(arr)) op.gather = &_k.arrays.at(arr);
            op.gatherUnit = pat == "unit";
        }
    }
}

std::vector<LaneOp::Beat> Hwacha::beatsFor(VectorOp &op, LaneOp &lo, unsigned k) {
    const Instr &ins = *op.ins;
    unsigned tb = _p.tlDataBytes;
    unsigned perRow = _p.nSlices() * op.rate;          // 每 bank 行元素数
    auto [s0, s1] = lo.stripRange(k);
    std::vector<LaneOp::Beat> out;
    auto bankOf = [&](unsigned e) { return ((e - s0) / perRow) % _p.nBanks; };
    // RTL（tlv 跟踪 + micro_lstride/sstride）：只有单位步长访存把落在同一个 16 B beat 里的元素合并成一个请求，
    // 跨步/索引访存每个元素一个请求（每拍一个）
    const bool mergeBeats = ins.mode == Mode::Unit;
    auto addElem = [&](uint64_t addr, unsigned e) {
        uint64_t b = addr / tb * tb;
        if (out.empty() || out.back().addr != b || !mergeBeats) out.push_back(LaneOp::Beat{b, {}});
        auto &bk = out.back().banks;
        unsigned bank = bankOf(e);
        if (std::find(bk.begin(), bk.end(), bank) == bk.end()) bk.push_back(bank);
    };
    unsigned r = op.rate;
    if (op.trace && !op.trace->scalar && !op.trace->mem.empty()) {
        // 执行驱动：直接用踪迹中每个活跃元素的地址（索引/原子访存尤其需要）
        std::map<unsigned, uint64_t> addrOf(op.trace->mem.begin(), op.trace->mem.end());
        for (unsigned i = k * r; i < std::min<unsigned>((k + 1) * r, lo.chunks.size()); ++i)
            for (unsigned e = lo.chunks[i].first; e < lo.chunks[i].second; ++e) {
                auto it = addrOf.find(e);
                if (it == addrOf.end()) continue;
                for (unsigned j = 0; j <= ins.seglen; ++j) {
                    uint64_t a = it->second + uint64_t(j) * ins.elsize;
                    for (uint64_t x = a; x < a + ins.elsize; x += tb) addElem(x, e);
                    if ((a + ins.elsize - 1) / tb != a / tb) addElem(a + ins.elsize - 1, e);
                }
            }
        return out;
    }
    if (ins.mode == Mode::Unit || ins.mode == Mode::Stride) {
        uint64_t step = op.strideBytes;
        for (unsigned i = k * r; i < std::min<unsigned>((k + 1) * r, lo.chunks.size()); ++i)
            for (unsigned e = lo.chunks[i].first; e < lo.chunks[i].second; ++e)
                for (unsigned j = 0; j <= ins.seglen; ++j) {
                    if (!op.activeAt(e)) continue;
                    uint64_t a = op.base + uint64_t(e) * step + uint64_t(j) * ins.elsize;
                    if (ins.mode == Mode::Unit) { for (uint64_t x = a; x < a + ins.elsize; x += tb) addElem(x, e); if ((a + ins.elsize - 1) / tb != a / tb) addElem(a + ins.elsize - 1, e); }
                    else addElem(a, e);
                }
        return out;
    }
    std::mt19937_64 rng(op.id * 1000003ull + lo.lane * 7919ull + k);
    for (unsigned i = k * r; i < std::min<unsigned>((k + 1) * r, lo.chunks.size()); ++i)
        for (unsigned e = lo.chunks[i].first; e < lo.chunks[i].second; ++e) {
            uint64_t a;
            if (!op.gather) a = 0xA0000000ull + (rng() % (1u << 20)) * ins.elsize;
            else if (op.gatherUnit) a = op.gather->base + uint64_t(e % op.gather->n) * ins.elsize;
            else a = op.gather->base + (rng() % op.gather->n) * ins.elsize;
            addElem(a, e);
        }
    return out;
}

// ---- 主序列器退休 ----
void Hwacha::retireStep(Cycles now) {
    for (VectorOp *op : _inflight) {
        if (op->complete) continue;
        bool all = true;
        for (auto &lo : op->lanes) if (!lo->finished || (lo->finishCycle != NoCycle && lo->finishCycle > now)) { all = false; break; }
        if (all) {
            op->complete = true; op->completeCycle = now;
            _slotsUsed -= op->slots;
            op->block->pendingOps--;
            maybeAck(op->block);
        }
    }
    _inflight.erase(std::remove_if(_inflight.begin(), _inflight.end(), [](VectorOp *o) { return o->complete; }), _inflight.end());
    for (auto &l : _lanes) l->retireStep(now);
}

// ---- 每周期 ----
void Hwacha::tick() {
    Cycles now = curCycle();
    for (auto &l : _lanes) { l->vluStep(now); l->memCompletionStep(now); }
    retireStep(now);
    for (auto &l : _lanes) {
        l->vmuStep(now);
        ++vmuCycle[l->vmuReason];
        bool issued = l->schedule(now);
        ++laneCycle[issued ? "issue" : l->lastReason];
    }
    scalarStep(now);
    ctrlStep(now);
    if (_vru) _vru->step(now);
    _occSum += _slotsUsed;
    uint64_t progress = _instrsIssued;
    for (auto &l : _lanes) progress += l->stripsIssued + l->loadBeats + l->storeBeats + l->beatsReturned;
    if (progress != _lastProgress) { _lastProgress = progress; _lastProgressCycle = now; }
    else if (now - _lastProgressCycle > 200000) {
        std::ostringstream os;
        os << "no progress for 200000 cycles at cycle " << now << "; state dump:\n";
        os << "  vfActive=" << _vfActive << " pc=" << _pc << " slotsUsed=" << _slotsUsed << " vcmdq=" << _vcmdq.size() << " inflight=" << _inflight.size() << "\n";
        for (auto &l : _lanes) {
            os << "  lane vmu=" << l->vmuReason << " lastReason=" << l->lastReason << " outstanding=" << l->outstanding()
               << " vldq=" << l->vldqSize() << " vsdqUsed=" << l->vsdqUsed() << " portBlocked=" << l->portBlocked() << "\n";
            for (LaneOp *b : l->ops) {
                unsigned k = b->nstrips ? std::min<unsigned>(b->nextDone, b->nstrips - 1) : 0;
                os << "    op" << b->op->id << " '" << b->op->ins->text << "' nstrips=" << b->nstrips << " nextStrip=" << b->nextStrip
                   << " nextDone=" << b->nextDone << " stripsSent=" << b->stripsSent << " finished=" << b->finished
                   << " ret=" << (b->beatsReturned.empty() ? 0 : b->beatsReturned[k])
                   << " wr=" << (b->beatsWritten.empty() ? 0 : b->beatsWritten[k])
                   << " nb=" << (b->beats.empty() ? 0 : b->beats[k].size()) << "\n";
            }
        }
        sim::fatal(os.str());
    }
    bool finished = _ctrlDone && _ctrlCmds.empty() && _vcmdq.empty() && !_vfActive && _inflight.empty() && !_pendingBranch
                    && (!_vru || _vru->idle());
    if (finished) { _done = true; _endCycle = now + 1; return; }
    sim::mainEventQueue().schedule(&_tickEvent, clockEdge(1));
}

// ---- 报告 ----
static std::string pct(double a, double b) { std::ostringstream os; os << std::fixed << std::setprecision(1) << (b > 0 ? 100.0 * a / b : 0.0) << "%"; return os.str(); }

std::string Hwacha::reportText() const {
    std::ostringstream os;
    double c = std::max<double>(1, _endCycle);
    uint64_t strips = 0, lb = 0, sb = 0, rpb = 0, fmaBusy = 0;
    for (auto &l : _lanes) { strips += l->stripsIssued; lb += l->loadBeats; sb += l->storeBeats; rpb += l->readPortBusy; auto it = l->fuBusy.find("fma"); if (it != l->fuBusy.end()) fmaBusy += it->second; }
    os << "cycles            : " << _endCycle << "\n";
    os << "elements          : " << _n << "  (" << _itersTotal << " stripmine iterations, maxvl=" << _maxvl << ")\n";
    os << "cycles / element  : " << std::fixed << std::setprecision(3) << c / std::max<double>(1, _n) << "\n";
    os << "GFLOPS @" << _p.freqGhz << " GHz : " << std::setprecision(2) << 2.0 * _fmaElems / c * _p.freqGhz << "   (FMA elements " << _fmaElems << ")\n";
    os << "FMA utilization   : " << pct(fmaBusy, c * _p.nLanes * _p.nFmaUnits) << "\n";
    os << "VRF read port util: " << pct(rpb, c * _p.nLanes) << "\n";
    os << "memory bandwidth  : " << std::setprecision(2) << 16.0 * (lb + sb) / c << " B/cycle (load beats " << lb << ", store beats " << sb << ")\n";
    os << "sequencer occupancy: " << std::setprecision(2) << _occSum / c << " / " << _p.nSeqEntries << " slots\n";
    os << "instructions issued: " << _instrsIssued << " (vector ops " << _vectorOps << ", strips " << strips << ")\n";
    auto dumpMap = [&](const char *title, const std::map<std::string, uint64_t> &m, double denom) {
        os << title << "\n";
        std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (auto &[k, n] : v) os << "    " << std::left << std::setw(12) << k << std::right << std::setw(7) << pct(n, denom) << "\n";
    };
    dumpMap("lane cycles by state:", laneCycle, c * _p.nLanes);
    dumpMap("scalar unit cycles by state:", scalarCycle, c);
    dumpMap("VMU cycles by state:", vmuCycle, c * _p.nLanes);
    dumpMap("control thread cycles by state:", ctrlCycle, c);
    if (_vru) { os << "VRU:"; for (auto &[k, v] : _vru->stats) os << " " << k << "=" << v; os << "\n"; }
    for (auto &w : _warnings) os << "warning: " << w << "\n";
    return os.str();
}

std::string Hwacha::reportJson() const {
    std::ostringstream os;
    double c = std::max<double>(1, _endCycle);
    uint64_t lb = 0, sb = 0, fmaBusy = 0, strips = 0;
    for (auto &l : _lanes) { lb += l->loadBeats; sb += l->storeBeats; strips += l->stripsIssued; auto it = l->fuBusy.find("fma"); if (it != l->fuBusy.end()) fmaBusy += it->second; }
    os << "{\n  \"cycles\": " << _endCycle << ",\n  \"n_elems\": " << _n << ",\n  \"n_lanes\": " << _p.nLanes
       << ",\n  \"max_vlen\": " << _maxvl << ",\n  \"iterations\": " << _itersTotal
       << ",\n  \"cycles_per_elem\": " << c / std::max<double>(1, _n)
       << ",\n  \"gflops\": " << 2.0 * _fmaElems / c * _p.freqGhz
       << ",\n  \"fma_util\": " << fmaBusy / (c * _p.nLanes * _p.nFmaUnits)
       << ",\n  \"fma_elems\": " << _fmaElems << ",\n  \"load_beats\": " << lb << ",\n  \"store_beats\": " << sb
       << ",\n  \"strips_issued\": " << strips << ",\n  \"vector_ops\": " << _vectorOps
       << ",\n  \"mem_bw_bytes_per_cycle\": " << 16.0 * (lb + sb) / c << ",\n  \"seq_occupancy\": " << _occSum / c;
    auto dumpMap = [&](const char *key, const std::map<std::string, uint64_t> &m) {
        os << ",\n  \"" << key << "\": {"; bool first = true;
        for (auto &[k, v] : m) { os << (first ? "" : ", ") << "\"" << k << "\": " << v; first = false; }
        os << "}";
    };
    dumpMap("lane_cycle", laneCycle); dumpMap("scalar_cycle", scalarCycle); dumpMap("vmu_cycle", vmuCycle); dumpMap("ctrl_cycle", ctrlCycle);
    if (_vru) dumpMap("vru", _vru->stats);
    os << "\n}\n";
    return os.str();
}

}  // namespace hw
