#include "mem.hh"
#include <cmath>
#include <set>
#include <algorithm>
#include <cassert>

namespace mem {
using sim::curTick;

uint64_t Packet::nextId = 1;
Packet::Packet(Cmd c, Addr a, unsigned sz) : cmd(c), addr(a), size(sz), id(nextId++), created(curTick()) {}
Packet::~Packet() { for (auto *s : senderStates) delete s; }

// ------------------------------------------------------------------ Ports
void RequestPort::bind(ResponsePort &p) { _peer = &p; p._peer = this; }
bool RequestPort::sendTimingReq(Packet *pkt) { return _peer->recvTimingReq(pkt); }
void RequestPort::sendRespRetry() { _peer->recvRespRetry(); }
bool ResponsePort::sendTimingResp(Packet *pkt) { return _peer->recvTimingResp(pkt); }
void ResponsePort::sendReqRetry() { _peer->recvReqRetry(); }

// ------------------------------------------------------------------- Xbar
Xbar::Xbar(std::string n, Tick period, P p) : ClockedObject(std::move(n), period), _p(p) {
    for (int i = 0; i < p.nCpuPorts; ++i) _cpu.emplace_back(new CpuPort(name() + ".cpu" + std::to_string(i), *this, i));
    for (int i = 0; i < p.nMemPorts; ++i) _mem.emplace_back(new MemPort(name() + ".mem" + std::to_string(i), *this, i));
    _reqLayers.resize(p.nMemPorts);
    _respLayers.resize(p.nCpuPorts);
    for (int i = 0; i < p.nMemPorts; ++i) {
        Layer &l = _reqLayers[i];
        l.sendRetry = [this](int port) { _cpu[port]->sendReqRetry(); };
        l.release = new sim::EventFunctionWrapper([this, i] { release(_reqLayers[i]); }, name() + ".reqLayer" + std::to_string(i));
    }
    for (int i = 0; i < p.nCpuPorts; ++i) {
        Layer &l = _respLayers[i];
        l.sendRetry = [this](int port) { _mem[port]->sendRespRetry(); };
        l.release = new sim::EventFunctionWrapper([this, i] { release(_respLayers[i]); }, name() + ".respLayer" + std::to_string(i));
    }
}

void Xbar::regStats() {
    stReqs = &statScalar("requests");
    stRetries = &statScalar("peer_retries", "下游拒收次数");
    stLayerWait = &statScalar("layer_wait", "因 layer 繁忙而等待的次数");
}

bool Xbar::tryLayer(Layer &l, int port) {
    if (l.state != Layer::IDLE) {
        if (port != l.retryPort && std::find(l.waiting.begin(), l.waiting.end(), port) == l.waiting.end())
            l.waiting.push_back(port);
        return false;
    }
    return true;
}

void Xbar::occupy(Layer &l, Cycles cycles) {
    l.state = Layer::BUSY;
    l.busyUntil = clockEdge(cycles);
    sim::mainEventQueue().schedule(l.release, l.busyUntil);
}

void Xbar::release(Layer &l) {
    l.state = Layer::IDLE;
    if (!l.waiting.empty()) {
        int p = l.waiting.front();
        l.waiting.pop_front();
        l.sendRetry(p);
    }
}

bool Xbar::recvTimingReq(Packet *pkt, int src) {
    int dst = route(pkt->addr);
    Layer &l = _reqLayers[dst];
    if (!tryLayer(l, src)) { if (stLayerWait) ++*stLayerWait; return false; }
    auto *rs = new RouteState();
    rs->src = src;
    pkt->pushSenderState(rs);
    if (!_mem[dst]->sendTimingReq(pkt)) {
        pkt->popSenderState();
        delete rs;
        layerFailed(l, src);
        if (stRetries) ++*stRetries;
        return false;
    }
    if (stReqs) ++*stReqs;
    Cycles beats = (pkt->size + _p.widthBytes - 1) / _p.widthBytes;
    if (l.lastSrc >= 0 && l.lastSrc != src) l.credit += _p.switchPenalty;
    l.lastSrc = src;
    Cycles extra = 0;
    if (l.credit >= 1.0) { extra = (Cycles)l.credit; l.credit -= extra; }
    occupy(l, std::max<Cycles>(1, beats) + extra);
    return true;
}

void Xbar::recvReqRetry(int memIdx) {
    Layer &l = _reqLayers[memIdx];
    if (l.state == Layer::RETRY) {
        l.state = Layer::IDLE;
        int p = l.retryPort;
        l.retryPort = -1;
        l.sendRetry(p);
    }
}

bool Xbar::recvTimingResp(Packet *pkt, int memIdx) {
    auto *rs = static_cast<RouteState *>(pkt->senderStates.back());
    int dst = rs->src;
    Layer &l = _respLayers[dst];
    if (!tryLayer(l, memIdx)) return false;
    pkt->popSenderState();
    if (!_cpu[dst]->sendTimingResp(pkt)) {
        pkt->pushSenderState(rs);
        layerFailed(l, memIdx);
        return false;
    }
    delete rs;
    Cycles beats = (pkt->size + _p.widthBytes - 1) / _p.widthBytes;
    occupy(l, std::max<Cycles>(1, beats));
    return true;
}

void Xbar::recvRespRetry(int cpuIdx) {
    Layer &l = _respLayers[cpuIdx];
    if (l.state == Layer::RETRY) {
        l.state = Layer::IDLE;
        int p = l.retryPort;
        l.retryPort = -1;
        l.sendRetry(p);
    }
}

// ----------------------------------------------------------------- L2Bank
L2Bank::L2Bank(std::string n, Tick period, P p)
    : ClockedObject(std::move(n), period), _p(p),
      _cpu(name() + ".cpu_side", *this), _mem(name() + ".mem_side", *this),
      _respEvent([this] { sendResponses(); }, name() + ".resp"),
      _memEvent([this] { trySendMem(); }, name() + ".mem"),
      _cpuRetryEvent([this] { if (_needCpuRetry) { _needCpuRetry = false; _cpu.sendReqRetry(); } }, name() + ".cpuRetry") {
    _nSets = p.sizeBytes / (p.lineBytes * p.ways);
    _sets.assign(_nSets, std::vector<Line>(p.ways));
}

void L2Bank::regStats() {
    stHits = &statScalar("hits"); stMisses = &statScalar("misses");
    stMshrHits = &statScalar("mshr_hits", "命中在途行");
    stPrefetches = &statScalar("prefetches"); stPrefetchUsed = &statScalar("prefetch_used", "被需求访问用到的预取行");
    stWritebacks = &statScalar("writebacks"); stBlockedMshr = &statScalar("blocked_mshr");
    stBlockedTag = &statScalar("blocked_tag"); stAtomics = &statScalar("atomics");
}

L2Bank::Line *L2Bank::lookup(Addr la, unsigned &setIdx) {
    setIdx = unsigned((la / _p.lineBytes) % _nSets);
    for (auto &ln : _sets[setIdx])
        if (ln.tag == la && (ln.valid || ln.pending)) return &ln;
    return nullptr;
}

L2Bank::Line *L2Bank::allocate(Addr la) {
    unsigned setIdx = unsigned((la / _p.lineBytes) % _nSets);
    Line *victim = nullptr;
    for (auto &ln : _sets[setIdx]) {
        if (ln.pending) continue;
        if (!ln.valid) { victim = &ln; break; }
        if (!victim || ln.lastUsed < victim->lastUsed) victim = &ln;
    }
    if (!victim) return nullptr;
    if (victim->valid && victim->dirty) {
        auto *wb = new Packet(Packet::WriteReq, victim->tag, _p.lineBytes);
        wb->noResp = true;
        _memQueue.push_back(wb);
        ++_wbInQueue;
        if (stWritebacks) ++*stWritebacks;
    }
    victim->valid = false; victim->dirty = false; victim->prefetched = false;
    victim->pending = true; victim->tag = la;
    return victim;
}

void L2Bank::installLine(Addr addr, bool dirty) {
    Addr la = lineAddr(addr);
    unsigned setIdx;
    if (lookup(la, setIdx)) return;
    Line *v = allocate(la);
    if (!v) return;
    v->valid = true; v->pending = false; v->dirty = dirty; v->lastUsed = curTick();
    // allocate() 可能把被替换的脏行放进写回队列；预热阶段不需要
    while (!_memQueue.empty()) { delete _memQueue.back(); _memQueue.pop_back(); }
    _wbInQueue = 0;
}

// 一个 store beat 占用 store 通路的拍数：基础拍数 + 换行/部分写代价 + 并发行数查表（见 mem.hh 中 P 的说明）
double L2Bank::storeBeatCost(Addr la, unsigned size) {
    double cost = _p.storeCycles;
    if (la != _lastStoreLine) {
        cost += _p.storeSwitch;
        if (size < _p.lineBytes / 4) cost += _p.partialStoreSwitch;
    }
    _lastStoreLine = la;
    if (_p.storeConflict.empty()) return cost;
    // 并发行数：从当前行上一次出现到现在经过了几个不同的行（顺序流 0，L 条 lane 交错 L-1）；行首拍沿用上一拍
    double n = _lastStoreConcurrency;
    std::set<Addr> between; bool found = false;
    for (auto it = _recentStoreBeats.rbegin(); it != _recentStoreBeats.rend(); ++it) {
        if (*it == la) { found = true; break; }
        between.insert(*it);
    }
    if (found) n = (double)between.size() + 1.0;
    _lastStoreConcurrency = n;
    _recentStoreBeats.push_back(la);
    if (_recentStoreBeats.size() > _p.storeWindow) _recentStoreBeats.pop_front();
    const auto &t = _p.storeConflict;
    if (n <= t.front().first) return cost + t.front().second;
    if (n >= t.back().first) return cost + t.back().second;
    size_t i = 1; while (t[i].first < n) ++i;
    double x = std::log2(n), x0 = std::log2((double)t[i-1].first), x1 = std::log2((double)t[i].first);
    return cost + t[i-1].second + (t[i].second - t[i-1].second) * (x - x0) / (x1 - x0);
}

bool L2Bank::recvTimingReq(Packet *pkt) {
    Tick busy = std::max(_tagBusyUntil, pkt->isWrite() ? _storeBusyUntil : Tick(0));
    if (curTick() < busy) {
        _needCpuRetry = true;
        ++*stBlockedTag;
        if (!_cpuRetryEvent.scheduled()) sim::mainEventQueue().schedule(&_cpuRetryEvent, busy);
        return false;
    }
    Addr la = lineAddr(pkt->addr);
    if (pkt->isWrite() && pkt->cmd != Packet::PrefetchReq) {
        _storeCredit += storeBeatCost(la, pkt->size) - 1.0;
        Cycles extra = 0;
        if (_storeCredit >= 1.0) { extra = (Cycles)_storeCredit; _storeCredit -= (double)extra; }
        else if (_storeCredit < 0) _storeCredit = 0;
        _storeBusyUntil = clockEdge(1 + extra);
    }
    unsigned setIdx;
    Line *ln = lookup(la, setIdx);
    if (pkt->cmd == Packet::AtomicReq && !_p.supportsAtomics) sim::fatal("AMO to an L2 without atomic support");
    if (ln && ln->valid) {
        Tick ready = clockEdge(_p.tagLatency + _p.dataLatency + (pkt->cmd == Packet::AtomicReq ? 1 : 0));
        _tagBusyUntil = clockEdge(1);
        ln->lastUsed = curTick();
        if (pkt->isPrefetch()) { pkt->makeResponse(); pkt->size = 1; queueResponse(pkt, clockEdge(_p.tagLatency)); return true; }
        if (ln->prefetched) { ++*stPrefetchUsed; ln->prefetched = false; }
        ++*stHits;
        if (pkt->isWrite()) ln->dirty = true;
        if (pkt->cmd == Packet::AtomicReq) ++*stAtomics;
        if (pkt->needsResponse() && !pkt->noResp) {
            pkt->makeResponse();
            queueResponse(pkt, ready);
        } else {
            delete pkt;
        }
        return true;
    }
    auto it = _mshrs.find(la);
    if (it != _mshrs.end()) {
        Mshr *m = it->second;
        if (m->targets.size() >= _p.tgtsPerMshr) { _needCpuRetry = true; ++*stBlockedMshr; return false; }
        _tagBusyUntil = clockEdge(1);
        if (pkt->isPrefetch()) { m->targets.push_back(pkt); return true; }
        ++*stMshrHits;
        if (m->prefetched && !m->demandSeen) { m->demandSeen = true; ++*stPrefetchUsed; }
        m->targets.push_back(pkt);
        return true;
    }
    if (_mshrs.size() >= _p.mshrs || _wbInQueue >= _p.writeBuffers) { _needCpuRetry = true; ++*stBlockedMshr; return false; }
    Line *victim = allocate(la);
    if (!victim) { _needCpuRetry = true; ++*stBlockedMshr; return false; }
    _tagBusyUntil = clockEdge(1);
    auto *m = new Mshr();
    m->lineAddr = la; m->allocated = curTick();
    if (pkt->isPrefetch()) { m->prefetched = true; ++*stPrefetches; }
    else ++*stMisses;
    m->targets.push_back(pkt);
    _mshrs[la] = m;
    auto *fill = new Packet(Packet::ReadReq, la, _p.lineBytes);
    _memQueue.push_back(fill);
    trySendMem();
    return true;
}

void L2Bank::trySendMem() {
    while (!_memQueue.empty() && !_memBlocked) {
        Packet *pkt = _memQueue.front();
        if (!_mem.sendTimingReq(pkt)) { _memBlocked = true; return; }
        _memQueue.pop_front();
        if (pkt->noResp) --_wbInQueue;
    }
}

void L2Bank::recvReqRetry() { _memBlocked = false; trySendMem(); }

bool L2Bank::recvTimingResp(Packet *fill) {
    handleFill(fill);
    return true;
}

void L2Bank::handleFill(Packet *fill) {
    Addr la = lineAddr(fill->addr);
    auto it = _mshrs.find(la);
    if (it == _mshrs.end()) { delete fill; return; }
    Mshr *m = it->second;
    unsigned setIdx;
    Line *ln = lookup(la, setIdx);
    if (ln) {
        ln->valid = true; ln->pending = false; ln->lastUsed = curTick();
        ln->prefetched = m->prefetched && !m->demandSeen;
    }
    Cycles i = 0;
    for (Packet *t : m->targets) {
        if (t->isWrite() && ln) ln->dirty = true;
        if (t->needsResponse() && !t->noResp) {
            t->makeResponse();
            if (t->cmd == Packet::PrefetchResp) t->size = 1;
            queueResponse(t, clockEdge(_p.responseLatency + i));
            ++i;
        } else {
            delete t;
        }
    }
    _mshrs.erase(it);
    delete m;
    delete fill;
    if (_needCpuRetry && !_cpuRetryEvent.scheduled())
        sim::mainEventQueue().schedule(&_cpuRetryEvent, std::max(curTick(), _tagBusyUntil));
}

void L2Bank::queueResponse(Packet *pkt, Tick ready) {
    auto pos = _respQueue.end();
    while (pos != _respQueue.begin() && std::prev(pos)->ready > ready) --pos;
    _respQueue.insert(pos, QueuedResp{pkt, ready});
    if (!_respEvent.scheduled()) sim::mainEventQueue().schedule(&_respEvent, std::max(curTick(), _respQueue.front().ready));
    else if (_respEvent.when() > _respQueue.front().ready) sim::mainEventQueue().reschedule(&_respEvent, std::max(curTick(), _respQueue.front().ready));
}

void L2Bank::sendResponses() {
    while (!_respQueue.empty() && !_respBlocked) {
        QueuedResp &r = _respQueue.front();
        if (r.ready > curTick()) { sim::mainEventQueue().reschedule(&_respEvent, r.ready); return; }
        if (!_cpu.sendTimingResp(r.pkt)) { _respBlocked = true; return; }
        _respQueue.pop_front();
    }
}

void L2Bank::recvRespRetry() { _respBlocked = false; sendResponses(); }

// --------------------------------------------------------------- DRAMCtrl
DRAMCtrl::DRAMCtrl(std::string n, Tick tCK, P p, unsigned chan)
    : ClockedObject(std::move(n), tCK), _p(p), _chan(chan), _port(name() + ".port", *this),
      _nextReqEvent([this] { processNextReq(); }, name() + ".nextReq"),
      _respEvent([this] { sendResponses(); }, name() + ".resp"),
      _refreshEvent([this] { refreshTick(); }, name() + ".refresh") {
    _ranks.resize(p.ranks);
    for (auto &r : _ranks) r.banks.resize(p.banks);
}

void DRAMCtrl::regStats() {
    stReads = &statScalar("reads"); stWrites = &statScalar("writes");
    stRowHits = &statScalar("row_hits"); stRowMisses = &statScalar("row_misses");
    stReadBytes = &statScalar("read_bytes"); stWriteBytes = &statScalar("write_bytes");
    stTotReadLat = &statScalar("tot_read_latency_ticks", "读请求从入队到数据结束的总延迟");
    stQueueLat = &statScalar("tot_queue_latency_ticks");
    stBusBusy = &statScalar("bus_busy_ticks"); stRetries = &statScalar("queue_full_retries");
}

void DRAMCtrl::startup() {
    sim::mainEventQueue().schedule(&_refreshEvent, curTick() + cyclesToTicks(_p.tREFI));
}

void DRAMCtrl::refreshTick() {
    Tick now = curTick();
    for (auto &rk : _ranks) {
        rk.refreshUntil = now + cyclesToTicks(_p.tRFC);
        for (auto &b : rk.banks) { b.openRow = -1; b.actAllowedAt = std::max(b.actAllowedAt, rk.refreshUntil); }
    }
    sim::mainEventQueue().schedule(&_refreshEvent, now + cyclesToTicks(_p.tREFI));
}

void DRAMCtrl::decode(Addr a, unsigned &rank, unsigned &bank, uint64_t &row) const {
    Addr local = (a / ((Addr)_p.channelInterleave * _p.nChannels)) * _p.channelInterleave + (a % _p.channelInterleave);
    Addr rowIdx = local / _p.rowBytes;
    bank = unsigned(rowIdx % _p.banks);
    rank = unsigned((rowIdx / _p.banks) % _p.ranks);
    row = rowIdx / _p.banks / _p.ranks;
}

bool DRAMCtrl::recvTimingReq(Packet *pkt) {
    Req r;
    r.pkt = pkt; r.addr = pkt->addr; r.isRead = pkt->isRead(); r.entry = curTick();
    r.bursts = std::max(1u, (pkt->size + _p.burstBytes - 1) / _p.burstBytes);
    decode(pkt->addr, r.rank, r.bank, r.row);
    if (r.isRead) {
        if (_readQ.size() >= _p.readQueue) { _retryRead = true; ++*stRetries; return false; }
        _readQ.push_back(r);
    } else {
        if (_writeQ.size() >= _p.writeQueue) { _retryWrite = true; ++*stRetries; return false; }
        if (pkt->needsResponse() && !pkt->noResp) {
            // 写请求进入队列即应答（gem5 早应答语义）
            pkt->makeResponse();
            _respQ.push_back(Resp{pkt, clockEdge(_p.frontendLatency)});
            if (!_respEvent.scheduled()) sim::mainEventQueue().schedule(&_respEvent, _respQ.back().ready);
            r.pkt = nullptr;
        }
        _writeQ.push_back(r);
    }
    if (!_nextReqEvent.scheduled()) sim::mainEventQueue().schedule(&_nextReqEvent, clockEdge(_p.frontendLatency));
    return true;
}

Tick DRAMCtrl::estimateCol(const Req &r, bool &rowHit) const {
    auto C = [this](unsigned c) { return cyclesToTicks(c); };
    const Rank &rk = _ranks[r.rank];
    const Bank &b = rk.banks[r.bank];
    Tick base = std::max(r.entry, rk.refreshUntil);
    Tick colAt;
    rowHit = b.openRow == (int64_t)r.row;
    if (rowHit) colAt = std::max(base, b.colAllowedAt);
    else {
        Tick actBase = base;
        if (b.openRow != -1) actBase = std::max(base, b.preAllowedAt) + C(_p.tRP);
        Tick actAt = std::max({actBase, b.actAllowedAt, rk.actAllowedAt});
        if (rk.actTicks.size() >= 4) actAt = std::max(actAt, rk.actTicks.front() + C(_p.tFAW));
        colAt = actAt + C(_p.tRCD);
    }
    colAt = std::max({colAt, curTick(), _nextColAllowedAt});
    if (r.isRead) colAt = std::max({colAt, b.rdAllowedAt, _lastWriteEnd + C(_p.tWTR)});
    else colAt = std::max({colAt, b.wrAllowedAt, _lastReadEnd + C(_p.tRTW)});
    return colAt;
}

bool DRAMCtrl::chooseNext(std::deque<Req> &q, size_t &idx, Tick &earliest) {
    // FR-FCFS：在所有排队请求中选列命令可最早发出者；行命中优先，同时刻取最老
    if (q.empty()) return false;
    Tick best = sim::MaxTick; bool bestHit = false; idx = 0;
    for (size_t i = 0; i < q.size(); ++i) {
        bool hit; Tick c = estimateCol(q[i], hit);
        if (c < best || (c == best && hit && !bestHit)) { best = c; bestHit = hit; idx = i; }
    }
    earliest = best;
    return true;
}

Tick DRAMCtrl::doAccess(Req &r) {
    auto C = [this](unsigned c) { return cyclesToTicks(c); };
    Rank &rk = _ranks[r.rank];
    Bank &b = rk.banks[r.bank];
    Tick now = curTick();
    Tick base = std::max(r.entry, rk.refreshUntil);
    Tick colAt;
    if (b.openRow == (int64_t)r.row) {
        ++*stRowHits;
        colAt = std::max(base, b.colAllowedAt);
    } else {
        ++*stRowMisses;
        Tick actBase = base;
        if (b.openRow != -1) {
            Tick preAt = std::max(base, b.preAllowedAt);
            actBase = preAt + C(_p.tRP);
        }
        Tick actAt = std::max({actBase, b.actAllowedAt, rk.actAllowedAt});
        while (!rk.actTicks.empty() && rk.actTicks.front() + C(_p.tFAW) <= actAt) rk.actTicks.pop_front();
        if (rk.actTicks.size() >= 4) { actAt = std::max(actAt, rk.actTicks.front() + C(_p.tFAW)); rk.actTicks.pop_front(); }
        rk.actTicks.push_back(actAt);
        rk.actAllowedAt = actAt + C(_p.tRRD);
        b.openRow = (int64_t)r.row;
        b.actAllowedAt = actAt + C(_p.tRC);
        b.preAllowedAt = actAt + C(_p.tRAS);
        b.colAllowedAt = actAt + C(_p.tRCD);
        colAt = b.colAllowedAt;
    }
    colAt = std::max({colAt, now, _nextColAllowedAt});
    if (r.isRead) colAt = std::max({colAt, b.rdAllowedAt, _lastWriteEnd + C(_p.tWTR)});
    else colAt = std::max({colAt, b.wrAllowedAt, _lastReadEnd + C(_p.tRTW)});
    Tick burst = C(_p.tBURST) * r.bursts;
    Tick dataStart = colAt + C(r.isRead ? _p.tCL : _p.tCWL);
    Tick dataEnd = dataStart + burst;
    _nextColAllowedAt = colAt + std::max(burst, C(_p.tCCD));
    b.rdAllowedAt = b.wrAllowedAt = _nextColAllowedAt;
    if (r.isRead) { b.preAllowedAt = std::max(b.preAllowedAt, colAt + C(_p.tRTP)); _lastReadEnd = dataEnd; }
    else { b.preAllowedAt = std::max(b.preAllowedAt, dataEnd + C(_p.tWR)); _lastWriteEnd = dataEnd; }
    _lastCmdAt = colAt;
    *stBusBusy += burst;
    *stQueueLat += colAt - r.entry;
    return dataEnd;
}

void DRAMCtrl::processNextReq() {
    if (_writeQ.size() >= _p.writeHigh || (_readQ.empty() && _writeQ.size() > _p.writeLow)) _writeMode = true;
    if (_writeMode && (_writeQ.size() <= _p.writeLow && !_readQ.empty())) _writeMode = false;
    if (_writeMode && _writeQ.empty()) _writeMode = false;
    if (!_writeMode && _readQ.empty() && !_writeQ.empty()) _writeMode = true;
    std::deque<Req> &q = _writeMode ? _writeQ : _readQ;
    if (q.empty()) return;
    size_t idx; Tick earliest;
    chooseNext(q, idx, earliest);
    Req r = q[idx];
    q.erase(q.begin() + idx);
    Tick dataEnd = doAccess(r);
    if (r.isRead) {
        ++*stReads; *stReadBytes += r.pkt->size;
        *stTotReadLat += dataEnd - r.entry;
        r.pkt->makeResponse();
        Tick ready = dataEnd + cyclesToTicks(_p.backendLatency);
        auto pos = _respQ.end();
        while (pos != _respQ.begin() && std::prev(pos)->ready > ready) --pos;
        _respQ.insert(pos, Resp{r.pkt, ready});
        if (!_respEvent.scheduled()) sim::mainEventQueue().schedule(&_respEvent, std::max(curTick(), _respQ.front().ready));
        else if (_respEvent.when() > _respQ.front().ready) sim::mainEventQueue().reschedule(&_respEvent, std::max(curTick(), _respQ.front().ready));
    } else {
        ++*stWrites; *stWriteBytes += r.pkt ? r.pkt->size : _p.lineBytes;
        if (r.pkt) delete r.pkt;
    }
    if (_retryRead && _readQ.size() < _p.readQueue) { _retryRead = false; _port.sendReqRetry(); }
    else if (_retryWrite && _writeQ.size() < _p.writeQueue) { _retryWrite = false; _port.sendReqRetry(); }
    if (!_readQ.empty() || !_writeQ.empty()) {
        Tick next = std::max(clockEdge(1), _nextColAllowedAt);
        if (!_nextReqEvent.scheduled()) sim::mainEventQueue().schedule(&_nextReqEvent, next);
    }
}

void DRAMCtrl::sendResponses() {
    while (!_respQ.empty() && !_respBlocked) {
        Resp &r = _respQ.front();
        if (r.ready > curTick()) { sim::mainEventQueue().reschedule(&_respEvent, r.ready); return; }
        if (!_port.sendTimingResp(r.pkt)) { _respBlocked = true; return; }
        _respQ.pop_front();
    }
}

void DRAMCtrl::recvRespRetry() { _respBlocked = false; sendResponses(); }

}  // namespace mem
