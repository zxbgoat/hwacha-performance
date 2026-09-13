"""strip 粒度的 Hwacha 周期级性能模拟器。

建模范围（对应 docs/05-microarchitecture-overview.md 的指令生命周期）：
  控制线程(Rocket) → VCMDQ/VRCMDQ → 标量单元取指/译码/发射 → 主序列器槽位
  → 每 lane 的序列器调度（RAW/WAR/WAW、读写口、共享功能单元与锁存器占用、chaining）
  → 展开器/systolic bank（用资源时间线表示）→ 定延迟/变延迟功能单元
  → DCC(VSU/VGU/VLU) ↔ VMU（在途 beat 上限、TLB、128 位端口）↔ L2/DRAM
  → VRU 预取与节流。
"""
from __future__ import annotations
from collections import deque, OrderedDict
from dataclasses import dataclass, field
import heapq
import math
import random
from typing import Optional

from .config import HwachaConfig, MemoryConfig
from .isa import Instr, stages_for, LATCH_GROUPS
from .program import Kernel
from .hvl import VCfg, max_vlen, rate_of
from .memory import MemorySystem, MemStats


# ----------------------------------------------------------------------------
# 工具：资源时间线
# ----------------------------------------------------------------------------
class Timeline:
    """按周期占用的资源（SRAM 读口、写口、功能单元、锁存器）。"""
    __slots__ = ('busy', 'last_prune')

    def __init__(self):
        self.busy: set[int] = set()
        self.last_prune = 0

    def free(self, t: int, dur: int = 1) -> bool:
        b = self.busy
        for i in range(dur):
            if (t + i) in b:
                return False
        return True

    def reserve(self, t: int, dur: int = 1) -> None:
        b = self.busy
        for i in range(dur):
            b.add(t + i)

    def first_free(self, t: int) -> int:
        while t in self.busy:
            t += 1
        return t

    def prune(self, now: int) -> None:
        if now - self.last_prune > 4096:
            self.busy = {x for x in self.busy if x >= now - 2}
            self.last_prune = now


# ----------------------------------------------------------------------------
# 向量操作与 lane 内状态
# ----------------------------------------------------------------------------
class VectorOp:
    __slots__ = ('id', 'ins', 'block', 'issue_time', 'vl', 'rate', 'E', 'lanes',
                 'slots', 'deps', 'reads', 'writes', 'complete_time', 'unit',
                 'lat', 'stride_bytes', 'base', 'gather', 'seed')

    def __init__(self, oid: int, ins: Instr, block: 'Block', vl: int, rate: int, E: int):
        self.id = oid
        self.ins = ins
        self.block = block
        self.issue_time = 0
        self.vl = vl
        self.rate = rate
        self.E = E
        self.lanes: list[LaneOp] = []
        self.slots = ins.slots
        self.deps: list[tuple[VectorOp, set[str]]] = []
        self.reads = ins.reads
        self.writes = set(ins.dst_regs()) if ins.writes_vrf else ins.writes
        self.complete_time: Optional[int] = None
        self.unit = ''
        self.lat = 0
        self.stride_bytes = 0
        self.base = 0
        self.gather = None
        self.seed = oid

    @property
    def complete(self) -> bool:
        return self.complete_time is not None


class LaneOp:
    """VectorOp 在某个 lane 中的执行状态。"""
    __slots__ = ('op', 'lane', 'chunks', 'nstrips', 'next_strip', 'issue', 'read_time',
                 'done', 'next_done', 'beats', 'strip_ptr', 'beat_ptr', 'completions',
                 'data_ready', 'addr_ready', 'strips_sent', 'vmu_start', 'finished',
                 'finish_time')

    def __init__(self, op: VectorOp, lane: int, chunks: list[tuple[int, int]]):
        self.op = op
        self.lane = lane
        self.chunks = chunks                       # [(e0, e1)] 每块 ≤ n_strip 个元素
        self.nstrips = max(1, math.ceil(len(chunks) / op.rate)) if chunks else 0
        self.next_strip = 0
        self.issue: list[Optional[int]] = [None] * self.nstrips
        self.read_time: list[Optional[int]] = [None] * self.nstrips
        self.done: list[Optional[int]] = [None] * self.nstrips
        self.next_done = 0
        self.beats: list[list[int]] = []
        self.strip_ptr = 0
        self.beat_ptr = 0
        self.completions: list[list[int]] = []
        self.data_ready: list[Optional[int]] = [None] * self.nstrips
        self.addr_ready: list[Optional[int]] = [None] * self.nstrips
        self.strips_sent = 0
        self.vmu_start = 0
        self.finished = self.nstrips == 0
        self.finish_time: Optional[int] = 0 if self.nstrips == 0 else None

    def strip_elems(self, k: int) -> list[tuple[int, int]]:
        r = self.op.rate
        return self.chunks[k * r:(k + 1) * r]

    def strip_range(self, k: int) -> tuple[int, int]:
        ch = self.strip_elems(k)
        return ch[0][0], ch[-1][1]

    def n_elems(self, k: int) -> int:
        return sum(e1 - e0 for e0, e1 in self.strip_elems(k))


class Block:
    __slots__ = ('id', 'vl', 'pending_ops', 'stopped', 'bytes', 'acked', 'start_time')

    def __init__(self, bid: int, vl: int):
        self.id = bid
        self.vl = vl
        self.pending_ops = 0
        self.stopped = False
        self.bytes = 0
        self.acked = False
        self.start_time = 0


# ----------------------------------------------------------------------------
# 统计
# ----------------------------------------------------------------------------
@dataclass
class SimStats:
    cycles: int = 0
    n_lanes: int = 1
    freq_ghz: float = 1.0
    n_elems: int = 0
    iterations: int = 0
    max_vlen: int = 0
    instrs_issued: int = 0
    vector_ops: int = 0
    strips_issued: int = 0
    fma_elems: int = 0
    alu_elems: int = 0
    mem_elems: int = 0
    load_beats: int = 0
    store_beats: int = 0
    fu_busy: dict = field(default_factory=dict)          # 单元 -> 占用周期（所有 lane 合计）
    read_port_busy: int = 0
    write_port_busy: int = 0
    lane_cycle: dict = field(default_factory=dict)       # 原因 -> 周期（所有 lane 合计）
    scalar_cycle: dict = field(default_factory=dict)     # 标量单元原因 -> 周期
    ctrl_cycle: dict = field(default_factory=dict)       # 控制线程原因 -> 周期
    vmu_cycle: dict = field(default_factory=dict)        # VMU 原因 -> 周期（所有 lane 合计）
    vru: dict = field(default_factory=dict)
    mem: Optional[MemStats] = None
    seq_occupancy: float = 0.0
    warnings: list = field(default_factory=list)

    # ---- 派生指标 ----
    @property
    def cycles_per_elem(self) -> float:
        return self.cycles / max(1, self.n_elems)

    @property
    def fma_util(self) -> float:
        n_units = self.fu_busy.get('_n_fma', 2)
        return self.fu_busy.get('fma', 0) / max(1, self.cycles * self.n_lanes * n_units)

    @property
    def gflops(self) -> float:
        return 2.0 * self.fma_elems / max(1, self.cycles) * self.freq_ghz

    @property
    def read_port_util(self) -> float:
        return self.read_port_busy / max(1, self.cycles * self.n_lanes)

    @property
    def mem_bw_bytes_per_cycle(self) -> float:
        return 16.0 * (self.load_beats + self.store_beats) / max(1, self.cycles)

    def to_dict(self) -> dict:
        d = {
            'cycles': self.cycles, 'n_lanes': self.n_lanes, 'n_elems': self.n_elems,
            'iterations': self.iterations, 'max_vlen': self.max_vlen,
            'cycles_per_elem': self.cycles_per_elem, 'gflops': self.gflops,
            'fma_util': self.fma_util, 'read_port_util': self.read_port_util,
            'mem_bw_bytes_per_cycle': self.mem_bw_bytes_per_cycle,
            'instrs_issued': self.instrs_issued, 'vector_ops': self.vector_ops,
            'strips_issued': self.strips_issued, 'fma_elems': self.fma_elems,
            'load_beats': self.load_beats, 'store_beats': self.store_beats,
            'fu_busy': {k: v for k, v in self.fu_busy.items() if not k.startswith('_')},
            'lane_cycle': dict(self.lane_cycle), 'scalar_cycle': dict(self.scalar_cycle),
            'ctrl_cycle': dict(self.ctrl_cycle), 'vmu_cycle': dict(self.vmu_cycle),
            'vru': dict(self.vru), 'seq_occupancy': self.seq_occupancy,
            'warnings': list(self.warnings),
        }
        if self.mem:
            d['mem'] = {k: getattr(self.mem, k) for k in vars(self.mem)}
        return d

    def report(self) -> str:
        L = []
        c = max(1, self.cycles)
        L.append(f"cycles            : {self.cycles}")
        L.append(f"elements          : {self.n_elems}  ({self.iterations} stripmine iterations, maxvl={self.max_vlen})")
        L.append(f"cycles / element  : {self.cycles_per_elem:.3f}")
        L.append(f"GFLOPS @{self.freq_ghz:g} GHz : {self.gflops:.2f}   (FMA elements {self.fma_elems})")
        L.append(f"FMA utilization   : {100 * self.fma_util:.1f}%")
        L.append(f"VRF read port util: {100 * self.read_port_util:.1f}%")
        L.append(f"memory bandwidth  : {self.mem_bw_bytes_per_cycle:.2f} B/cycle "
                 f"(load beats {self.load_beats}, store beats {self.store_beats})")
        L.append(f"sequencer occupancy: {self.seq_occupancy:.2f} / slots")
        L.append("lane cycles by state:")
        for k, v in sorted(self.lane_cycle.items(), key=lambda kv: -kv[1]):
            L.append(f"    {k:12s} {100 * v / (c * self.n_lanes):6.1f}%")
        L.append("scalar unit cycles by state:")
        for k, v in sorted(self.scalar_cycle.items(), key=lambda kv: -kv[1]):
            L.append(f"    {k:12s} {100 * v / c:6.1f}%")
        L.append("VMU cycles by state:")
        for k, v in sorted(self.vmu_cycle.items(), key=lambda kv: -kv[1]):
            L.append(f"    {k:12s} {100 * v / (c * self.n_lanes):6.1f}%")
        L.append("control thread cycles by state:")
        for k, v in sorted(self.ctrl_cycle.items(), key=lambda kv: -kv[1]):
            L.append(f"    {k:12s} {100 * v / c:6.1f}%")
        if self.mem:
            m = self.mem
            L.append(f"L2: hits {m.hits}, misses {m.misses}, pending-hits {m.pending_hits} "
                     f"(prefetch hits {m.prefetch_hits}), DRAM lines {m.dram_lines}, "
                     f"writebacks {m.dram_writebacks}, bank-queue {m.bank_queue_cycles}, "
                     f"tracker-wait {m.tracker_wait_cycles}")
        if self.vru:
            L.append("VRU: " + ", ".join(f"{k}={v}" for k, v in self.vru.items()))
        for w in self.warnings:
            L.append("warning: " + w)
        return "\n".join(L)


# ----------------------------------------------------------------------------
# lane：序列器调度 + VMU
# ----------------------------------------------------------------------------
class Lane:
    def __init__(self, sim: 'Simulator', lid: int):
        self.sim = sim
        self.cfg = sim.cfg
        self.id = lid
        self.ops: list[LaneOp] = []            # 按年龄
        self.read_port = Timeline()
        self.write_port = Timeline()
        self.pred_read = Timeline()
        self.pred_write = Timeline()
        self.units: dict[str, Timeline] = {u: Timeline() for u in
                                           ('fma0', 'fma1', 'imul', 'fconv', 'fcmp', 'vqu', 'vgu',
                                            'fdiv', 'idiv')}
        self.latches = [Timeline() for _ in range(6)]
        # VMU
        self.vmu_queue: deque[LaneOp] = deque()
        self.vmu_stall_until = 0
        self.outstanding: list[int] = []       # 在途 beat 完成时刻（堆）
        self.resp_port = Timeline()
        self.tlb: OrderedDict[int, None] = OrderedDict()
        self._port_credit = 0.0
        self.last_reason = 'empty'
        self.vmu_reason = 'idle'

    # ---- 冒险 ----
    def _strips_covering(self, a: LaneOp, e0: int, e1: int) -> range:
        if a.nstrips == 0:
            return range(0)
        U = self.cfg.n_strip
        nl = self.cfg.n_lanes
        r = a.op.rate
        k0 = ((e0 // U) // nl) // r
        k1 = (((e1 - 1) // U) // nl) // r
        return range(max(0, k0), min(a.nstrips - 1, k1) + 1)

    def hazard(self, b: LaneOp, k: int, t: int, write_time: Optional[int]) -> Optional[str]:
        if not b.op.deps:
            return None
        e0, e1 = b.strip_range(k)
        for A, kinds in b.op.deps:
            a = A.lanes[self.id]
            for ka in self._strips_covering(a, e0, e1):
                if 'raw' in kinds:
                    d = a.done[ka]
                    if d is None or d >= t:
                        return 'raw_mem' if A.ins.is_mem else 'raw'
                if 'waw' in kinds and write_time is not None:
                    d = a.done[ka]
                    if d is None or d >= write_time:
                        return 'waw'
                if 'war' in kinds:
                    rt = a.read_time[ka]
                    if rt is None or rt > t:
                        return 'war'
        return None

    def _load_write_gate(self, b: LaneOp, k: int, t: int) -> bool:
        """load 写回前的 WAW/WAR 检查。"""
        if not b.op.deps:
            return True
        e0, e1 = b.strip_range(k)
        for A, kinds in b.op.deps:
            if not ('waw' in kinds or 'war' in kinds):
                continue
            a = A.lanes[self.id]
            for ka in self._strips_covering(a, e0, e1):
                if 'waw' in kinds and (a.done[ka] is None or a.done[ka] >= t):
                    return False
                if 'war' in kinds and (a.read_time[ka] is None or a.read_time[ka] > t):
                    return False
        return True

    # ---- 调度 ----
    @staticmethod
    def _second_port_kind(ins) -> bool:
        """RTL 序列器的第二调度端口只服务 VSU / VGU / VQU 类操作。"""
        if ins.kind in ('store', 'amo', 'fdiv', 'idiv', 'rfirst', 'branch'):
            return True
        return ins.kind == 'load' and ins.mode == 'indexed'

    def schedule(self, t: int) -> bool:
        """每周期最多两个调度端口：主端口任意操作，第二端口只发 VSU/VGU/VQU 类操作。返回是否发射。"""
        cfg = self.cfg
        reason = 'empty' if not self.ops else 'drain'
        issued = False
        first = None
        for port in range(2):
            for b in self.ops:
                if b is first or b.finished or b.next_strip >= b.nstrips:
                    continue
                ins = b.op.ins
                kind = ins.kind
                if kind in ('load', 'pmem') and ins.mode != 'indexed':
                    continue                       # 单位/常量步长访存由 VMU 驱动
                if port == 1 and not self._second_port_kind(ins):
                    continue
                if kind == 'amo' or (kind == 'load' and ins.mode == 'indexed'):
                    kind = 'vgu'
                elif kind == 'store' and ins.mode == 'indexed':
                    kind = 'vgu_store'
                k = b.next_strip
                rp = b.op.ins.rp
                r = self._try_issue(b, k, kind, rp, t)
                if r is None:
                    b.next_strip += 1
                    b.issue[k] = t
                    b.read_time[k] = t
                    if b.next_strip == b.nstrips and kind not in ('store', 'vgu', 'vgu_store'):
                        b.finish_time = b.done[-1]
                    self.sim.stats.strips_issued += 1
                    issued = True
                    first = b
                    break
                if port == 0 and reason in ('drain', 'empty'):
                    reason = r
        if not issued:
            self.last_reason = reason
        return issued

    def _try_issue(self, b: LaneOp, k: int, kind: str, rp: int, t: int) -> Optional[str]:
        cfg = self.cfg
        op = b.op
        ins = op.ins
        st = self.sim.stats
        n_elems = b.n_elems(k)
        fop = t + rp + 1
        pred = ins.pred is not None
        # 读口 / 谓词口
        if rp and not self.read_port.free(t, rp):
            return 'rport'
        if pred and not self.pred_read.free(t):
            return 'pport'
        unit = None
        lat = 0
        write_time: Optional[int] = None
        occupancy = cfg.n_banks
        if kind in ('fma', 'imul', 'fconv', 'fcmp'):
            lat = stages_for(ins, cfg)
            cands = ('fma0', 'fma1') if kind == 'fma' else (kind,)
            if kind == 'fma' and cfg.n_fma_units == 1:
                cands = ('fma0',)
            fu_free = False
            for u in cands:
                if self.units[u].free(fop, occupancy):
                    fu_free = True
                    if all(self.latches[i].free(t + 1, rp + 1) for i in LATCH_GROUPS[u]):
                        unit = u
                        break
            if unit is None:
                return 'fu' if not fu_free else 'latch'
            write_time = fop + lat
        elif kind in ('alu', 'cmp', 'plu'):
            lat = stages_for(ins, cfg)
            write_time = fop + lat
        elif kind in ('fdiv', 'idiv', 'rfirst', 'branch'):
            u = 'vqu'
            if not self.units[u].free(fop, occupancy):
                return 'fu'
            if not all(self.latches[i].free(t + 1, rp + 1) for i in LATCH_GROUPS[u]):
                return 'latch'
            unit = u
            if kind in ('fdiv', 'idiv'):
                per = cfg.fdiv_cycles_per_elem if kind == 'fdiv' else cfg.idiv_cycles_per_elem
                div = self.units[kind]
                if not div.free(fop):
                    return 'fu'
                occ = per * n_elems
                write_time = div.first_free(fop) + occ + 2
            else:
                write_time = fop + cfg.n_banks + (cfg.branch_resolve_latency if kind == 'branch' else 4)
        elif kind == 'store':
            if k - b.strips_sent >= cfg.vsdq_strips:
                return 'vsdq'
        elif kind in ('vgu', 'vgu_store'):
            u = 'vgu'
            if not self.units[u].free(fop, occupancy):
                return 'fu'
            if not all(self.latches[i].free(t + 1, rp + 1) for i in LATCH_GROUPS[u]):
                return 'latch'
            unit = u
            if kind == 'vgu_store' and k - b.strips_sent >= cfg.vsdq_strips:
                return 'vsdq'
        # 写口
        if write_time is not None:
            if ins.writes_prf:
                if not self.pred_write.free(write_time):
                    return 'wport'
            elif ins.writes_vrf or kind in ('rfirst', 'branch'):
                if kind not in ('rfirst', 'branch') and not self.write_port.free(write_time):
                    return 'wport'
        # 数据冒险
        h = self.hazard(b, k, t, write_time)
        if h:
            return h
        # ---- 发射：预约资源 ----
        if rp:
            self.read_port.reserve(t, rp)
            st.read_port_busy += rp
        if pred:
            self.pred_read.reserve(t)
        if unit:
            self.units[unit].reserve(fop, occupancy)
            for i in LATCH_GROUPS[unit]:
                self.latches[i].reserve(t + 1, rp + 1)
            key = 'fma' if unit.startswith('fma') else unit
            st.fu_busy[key] = st.fu_busy.get(key, 0) + occupancy
            if kind in ('fdiv', 'idiv'):
                d = self.units[kind]
                s0 = d.first_free(fop)
                d.reserve(s0, write_time - 2 - s0)
                st.fu_busy[kind] = st.fu_busy.get(kind, 0) + (write_time - 2 - s0)
        if kind == 'fma':
            st.fma_elems += n_elems
        elif kind in ('alu', 'cmp'):
            st.alu_elems += n_elems
        if write_time is not None:
            if ins.writes_prf:
                self.pred_write.reserve(write_time)
            elif ins.writes_vrf:
                self.write_port.reserve(write_time)
                st.write_port_busy += 1
            b.done[k] = write_time
        if kind == 'store':
            b.data_ready[k] = t + 2
        elif kind == 'vgu_store':
            b.data_ready[k] = fop + 1
            b.addr_ready[k] = fop + 1
        elif kind == 'vgu':
            b.addr_ready[k] = fop + 1
            b.data_ready[k] = fop + 1
        if kind in ('rfirst', 'branch'):
            b.done[k] = write_time
        return None

    # ---- VMU ----
    def vmu_step(self, t: int) -> None:
        cfg = self.cfg
        st = self.sim.stats
        self.vmu_reason = 'idle'
        while self.outstanding and self.outstanding[0] <= t:
            heapq.heappop(self.outstanding)
        if t < self.vmu_stall_until:
            self.vmu_reason = 'tlb'
            return
        if self._port_credit >= 1.0:
            self._port_credit -= 1.0
            self.vmu_reason = 'port_occ'
            return
        if not self.vmu_queue:
            return
        b = self.vmu_queue[0]
        if b.nstrips == 0:
            self.vmu_queue.popleft()
            return
        if t < b.vmu_start:
            self.vmu_reason = 'issue_lat'
            return
        s = b.strip_ptr
        ins = b.op.ins
        if ins.is_store or ins.kind == 'amo' or ins.mode == 'indexed':
            need = b.data_ready[s] if (ins.is_store or ins.kind == 'amo') else b.addr_ready[s]
            if need is None or need > t:
                self.vmu_reason = 'wait_data' if ins.is_store else 'wait_addr'
                return
        if len(self.outstanding) >= cfg.n_vmt_entries:
            self.vmu_reason = 'vmt_full'
            return
        beats = b.beats[s]
        addr = beats[b.beat_ptr] * cfg.tl_data_bytes
        page = addr // self.sim.mcfg.page_bytes
        if page not in self.tlb:
            if len(self.tlb) >= self.sim.mcfg.tlb_entries:
                self.tlb.popitem(last=False)
            self.tlb[page] = None
            self.vmu_stall_until = t + self.sim.mcfg.tlb_miss_latency
            self.vmu_reason = 'tlb'
            return
        self.tlb.move_to_end(page)
        is_store = ins.is_store and ins.kind != 'amo'
        comp = self.sim.mem.request(t, addr, is_store=is_store or ins.kind == 'amo')
        if ins.is_load:
            comp = self.resp_port.first_free(comp)
            self.resp_port.reserve(comp)
            st.load_beats += 1
        else:
            st.store_beats += 1
        b.completions[s].append(comp)
        heapq.heappush(self.outstanding, comp)
        self._port_credit += (cfg.store_beat_cycles if (is_store or ins.kind == 'amo') else cfg.load_beat_cycles) - 1.0
        self.vmu_reason = 'busy'
        b.beat_ptr += 1
        if b.beat_ptr >= len(beats):
            b.beat_ptr = 0
            b.strip_ptr += 1
            b.strips_sent += 1
            if b.strip_ptr >= b.nstrips:
                self.vmu_queue.popleft()

    def mem_completion_step(self, t: int) -> None:
        """按顺序把已返回的 strip 标记完成（VLU 写回 / store 应答）。"""
        cfg = self.cfg
        for b in self.ops:
            ins = b.op.ins
            if b.finished or not ins.is_mem:
                continue
            while b.next_done < b.nstrips:
                k = b.next_done
                if k >= b.strips_sent or len(b.completions[k]) < len(b.beats[k]):
                    break
                ret = max(b.completions[k])
                if ret > t:
                    break
                if ins.is_load:
                    if not self._load_write_gate(b, k, t):
                        break
                    w = self.write_port.first_free(t + cfg.vlu_latency)
                    self.write_port.reserve(w)
                    self.sim.stats.write_port_busy += 1
                    b.done[k] = w
                else:
                    b.done[k] = t
                b.read_time[k] = b.issue[k] if b.issue[k] is not None else t
                b.next_done += 1
            if b.next_done >= b.nstrips:
                b.finished = True
                b.finish_time = max(d for d in b.done if d is not None) if b.done else t

    def retire_step(self, t: int) -> None:
        for b in self.ops:
            if b.finished:
                continue
            if b.op.ins.is_mem:
                continue
            if b.next_strip >= b.nstrips and b.done and all(d is not None for d in b.done) and t >= max(b.done):
                b.finished = True
                b.finish_time = max(b.done)
        self.ops = [b for b in self.ops if not (b.finished and b.op.complete)]

    def prune(self, t: int) -> None:
        for tl in (self.read_port, self.write_port, self.pred_read, self.pred_write, self.resp_port,
                   *self.units.values(), *self.latches):
            tl.prune(t)


# ----------------------------------------------------------------------------
# VRU
# ----------------------------------------------------------------------------
class VRU:
    def __init__(self, sim: 'Simulator'):
        self.sim = sim
        self.cfg = sim.cfg
        self.va: dict[str, int] = {}
        self.vl = 0
        self.cmdq: deque = deque()
        self.pf_queue: deque[int] = deque()
        self.outstanding: list[int] = []
        self.block_bytes: deque[int] = deque()
        self.runahead_bytes = 0
        self.blocks_seen = 0
        self.decoding: Optional[tuple[list[Instr], int, int]] = None   # (instrs, idx, vl)
        self.stats = {'prefetch_lines': 0, 'blocks_decoded': 0, 'blocks_skipped': 0,
                      'throttle_cycles': 0, 'outstanding_full_cycles': 0}
        self.seen_lines: OrderedDict[int, None] = OrderedDict()

    def enqueue(self, cmd: tuple) -> None:
        if len(self.cmdq) < self.cfg.cmdq_len:
            self.cmdq.append(cmd)

    def ack_block(self) -> None:
        if self.block_bytes:
            self.runahead_bytes -= self.block_bytes.popleft()

    def step(self, t: int) -> None:
        cfg = self.cfg
        # 预取发射
        while self.outstanding and self.outstanding[0] <= t:
            heapq.heappop(self.outstanding)
        if self.pf_queue:
            if len(self.outstanding) < cfg.vru_max_outstanding:
                line = self.pf_queue.popleft()
                arr = self.sim.mem.request(t, line * self.sim.mcfg.line_bytes, prefetch=True)
                heapq.heappush(self.outstanding, arr)
                self.stats['prefetch_lines'] += 1
            else:
                self.stats['outstanding_full_cycles'] += 1
        # 译码（每周期一条）
        if self.decoding is not None:
            instrs, idx, vl = self.decoding
            ins = instrs[idx]
            if ins.kind == 'stop':
                self.decoding = None
                return
            if ins.is_mem and ins.mode in ('unit', 'stride'):
                self._prefetch_for(ins, vl)
            self.decoding = (instrs, idx + 1, vl)
            return
        if self.runahead_bytes > cfg.vru_max_runahead_bytes:
            self.stats['throttle_cycles'] += 1
            return
        if not self.cmdq:
            return
        cmd = self.cmdq.popleft()
        if cmd[0] == 'vsetvl':
            self.vl = cmd[1]
        elif cmd[0] == 'vmca':
            self.va[cmd[1]] = cmd[2]
        elif cmd[0] == 'vf':
            self.blocks_seen += 1
            nbytes = self._block_bytes(self.sim.kernel.instrs, self.vl)
            self.block_bytes.append(nbytes)
            self.runahead_bytes += nbytes
            if self.blocks_seen <= cfg.vru_early_ignore:
                self.stats['blocks_skipped'] += 1
                return
            self.stats['blocks_decoded'] += 1
            self.decoding = (self.sim.kernel.instrs, 0, self.vl)

    def _block_bytes(self, instrs: list[Instr], vl: int) -> int:
        n = 0
        for ins in instrs:
            if ins.is_mem and ins.mode in ('unit', 'stride'):
                n += vl * ins.elsize * (ins.seglen + 1)
        return n

    def _prefetch_for(self, ins: Instr, vl: int) -> None:
        base_reg, stride_reg = self.sim.mem_regs(ins)
        base = self.va.get(base_reg)
        if base is None:
            return
        lb = self.sim.mcfg.line_bytes
        if ins.mode == 'unit':
            step = ins.elsize * (ins.seglen + 1)
            lo, hi = base // lb, (base + vl * step + lb - 1) // lb
            lines = range(lo, hi)
        else:
            stride = self.va.get(stride_reg, ins.elsize) if stride_reg else ins.elsize
            lines = OrderedDict.fromkeys((base + e * stride) // lb for e in range(vl))
        for ln in lines:
            if ln in self.seen_lines:
                continue
            self.seen_lines[ln] = None
            if len(self.seen_lines) > 4096:
                self.seen_lines.popitem(last=False)
            self.pf_queue.append(ln)


# ----------------------------------------------------------------------------
# 模拟器
# ----------------------------------------------------------------------------
class Simulator:
    def __init__(self, kernel: Kernel, cfg: Optional[HwachaConfig] = None,
                 mcfg: Optional[MemoryConfig] = None, n: Optional[int] = None):
        self.kernel = kernel
        self.cfg = cfg or HwachaConfig()
        self.mcfg = mcfg or MemoryConfig()
        self.n = n if n is not None else kernel.n
        self.mem = MemorySystem(self.mcfg)
        if self.mcfg.warm_l2:
            # 预热规则：被 va 指针引用的数组预热到 (n + offset + 16) 个元素 × 步长；仅被索引访存引用的数组整体预热
            for name, arr in kernel.arrays.items():
                refs = [d for d in kernel.va.values() if d.kind == 'ptr' and d.array == name]
                if refs:
                    nbytes = max((self.n + d.offset_elems + 16) * arr.elem * max(1, d.stride_elems) for d in refs)
                else:
                    nbytes = arr.nbytes
                nbytes = min(nbytes, arr.nbytes)
                for a in range(arr.base, arr.base + nbytes, self.mcfg.line_bytes):
                    self.mem.install(a, True)
        self.stats = SimStats(n_lanes=self.cfg.n_lanes, freq_ghz=self.cfg.freq_ghz)
        self.lanes = [Lane(self, i) for i in range(self.cfg.n_lanes)]
        self.vru = VRU(self) if self.cfg.build_vru else None
        self.maxvl = max_vlen(self.cfg, kernel.vcfg)
        self.stats.max_vlen = self.maxvl
        self._check_kernel()
        # 主序列器
        self.inflight: list[VectorOp] = []
        self.slots_used = 0
        self.next_op_id = 0
        self.blocks: list[Block] = []
        # 命令队列
        self.vcmdq: deque = deque()
        # 标量单元状态
        self.va: dict[str, int] = {}
        self.vl = 0
        self.vf_active = False
        self.pc = 0
        self.cur_block: Optional[Block] = None
        self.stall_until = 0
        self.scoreboard: dict[str, object] = {}    # vs -> ready time 或 VectorOp
        self.pending_branch: Optional[tuple[VectorOp, Instr]] = None
        self.branch_taken_count: dict[int, int] = {}
        self.pending_smu = False
        self.instrs = kernel.instrs
        self.rng = random.Random(1)
        # 控制线程
        self.ctrl_iter = 0
        self.ctrl_remaining = self.n
        self.ctrl_cmds: deque = deque()
        self.ctrl_busy_until = 0
        self.ctrl_done = False
        self.ctrl_cycles = kernel.ctrl_cycles if kernel.ctrl_cycles is not None else self.cfg.ctrl_cycles_per_iter
        self.iters_total = kernel.iters if kernel.iters else math.ceil(self.n / self.maxvl)
        self._va_offsets: dict[str, int] = {r: 0 for r in kernel.va}
        self._ctrl_seed()

    # ---- 检查 ----
    def _check_kernel(self) -> None:
        k = self.kernel
        for ins in k.instrs:
            for r in list(ins.reads) + list(ins.writes):
                if r.startswith('vv') and int(r[2:]) >= k.vcfg.nvv:
                    self.stats.warnings.append(f'{ins.text}: {r} exceeds vsetcfg (nvv={k.vcfg.nvv})')
                if r.startswith('vp') and int(r[2:]) >= k.vcfg.vp:
                    self.stats.warnings.append(f'{ins.text}: {r} exceeds vsetcfg (vp={k.vcfg.vp})')
            if ins.kind == 'amo' and not self.mcfg.l2_supports_amo:
                self.stats.warnings.append('AMO used but L2 does not support AMOs (open-source broadcast hub)')

    def mem_regs(self, ins: Instr) -> tuple[Optional[str], Optional[str]]:
        """返回 (基址寄存器, 步长寄存器)。"""
        if ins.kind == 'load' or (ins.kind == 'pmem' and ins.mnemonic == 'vpl'):
            srcs = ins.srcs
        else:
            srcs = ins.srcs[1:]          # 首操作数为 store 数据
        base = srcs[0] if srcs else None
        stride = srcs[1] if ins.mode == 'stride' and len(srcs) > 1 else None
        return base, stride

    # ---- 控制线程 ----
    def _ctrl_seed(self) -> None:
        self.ctrl_cmds.append(('vsetcfg',))
        for _ in range(0):
            pass
        self._ctrl_next_iter()

    def _ctrl_next_iter(self) -> None:
        if self.ctrl_iter >= self.iters_total or self.ctrl_remaining <= 0:
            self.ctrl_done = True
            return
        vl = min(self.ctrl_remaining, self.maxvl)
        self.ctrl_cmds.append(('vsetvl', vl))
        for reg, d in self.kernel.va.items():
            if d.kind == 'ptr':
                arr = self.kernel.arrays[d.array]
                addr = arr.base + d.offset_elems * arr.elem + self._va_offsets[reg]
                if d.advance:
                    # 越过数组末尾则回绕（表示同一数据块被后续分块复用）
                    self._va_offsets[reg] = (self._va_offsets[reg] + vl * arr.elem * d.stride_elems) % max(arr.elem, arr.nbytes)
            else:
                addr = d.value
            self.ctrl_cmds.append(('vmca', reg, addr))
        for _ in range(self.kernel.n_vmcs):
            self.ctrl_cmds.append(('vmcs',))
        self.ctrl_cmds.append(('vf', vl, self.ctrl_iter))
        self.ctrl_cmds.append(('_ctrl', self.ctrl_cycles))
        self.ctrl_remaining -= vl
        self.ctrl_iter += 1

    def ctrl_step(self, t: int) -> None:
        st = self.stats.ctrl_cycle
        if t < self.ctrl_busy_until:
            st['bookkeeping'] = st.get('bookkeeping', 0) + 1
            return
        if not self.ctrl_cmds:
            if self.ctrl_done:
                st['done'] = st.get('done', 0) + 1
                return
            self._ctrl_next_iter()
            if not self.ctrl_cmds:
                return
        cmd = self.ctrl_cmds[0]
        if cmd[0] == '_ctrl':
            self.ctrl_cmds.popleft()
            self.ctrl_busy_until = t + cmd[1]
            st['bookkeeping'] = st.get('bookkeeping', 0) + 1
            return
        if len(self.vcmdq) >= self.cfg.cmdq_len:
            st['vcmdq_full'] = st.get('vcmdq_full', 0) + 1
            return
        self.ctrl_cmds.popleft()
        self.vcmdq.append(cmd)
        if self.vru is not None and cmd[0] != 'vmcs':
            self.vru.enqueue(cmd)
        st['issue'] = st.get('issue', 0) + 1

    # ---- 标量单元 ----
    def scalar_step(self, t: int) -> None:
        st = self.stats.scalar_cycle

        def note(r):
            st[r] = st.get(r, 0) + 1

        if t < self.stall_until:
            note('latency')
            return
        if self.pending_branch is not None:
            op, ins = self.pending_branch
            if not op.complete or t < op.complete_time:
                note('branch')
                return
            self.pending_branch = None
            key = ins.line_no
            taken_budget = int(ins.annot.get('taken', 0) or 0)
            cnt = self.branch_taken_count.get(key, 0)
            if cnt < taken_budget:
                self.branch_taken_count[key] = cnt + 1
                self.pc = self.kernel.labels[ins.label]
            else:
                self.pc += 1
            note('issue')
            return
        if not self.vf_active:
            if not self.vcmdq:
                note('idle')
                return
            cmd = self.vcmdq.popleft()
            if cmd[0] == 'vsetvl':
                self.vl = cmd[1]
            elif cmd[0] == 'vmca':
                self.va[cmd[1]] = cmd[2]
            elif cmd[0] == 'vf':
                self.vf_active = True
                self.pc = 0
                self.branch_taken_count = {}
                self.cur_block = Block(len(self.blocks), self.vl)
                self.cur_block.start_time = t
                self.blocks.append(self.cur_block)
                self.stall_until = t + self.cfg.vf_fetch_latency + self.cfg.vf_block_overhead
            note('command')
            return
        ins = self.instrs[self.pc]
        # 共享寄存器 scoreboard
        for r in ins.srcs:
            if r.startswith('vs') and r in self.scoreboard:
                v = self.scoreboard[r]
                ready = v.complete_time if isinstance(v, VectorOp) else v
                if ready is None or ready > t:
                    note('scoreboard')
                    return
                del self.scoreboard[r]
        k = ins.kind
        if k == 'stop':
            self.vf_active = False
            self.cur_block.stopped = True
            self._maybe_ack(self.cur_block)
            note('issue')
            self.stats.instrs_issued += 1
            return
        if k == 'fence':
            if any(o.ins.is_mem and not o.complete for o in self.inflight) or self.pending_smu:
                note('fence')
                return
            self.pc += 1
            note('issue')
            return
        if k == 'scalar':
            self.pc += 1
            note('issue')
            self.stats.instrs_issued += 1
            return
        if k in ('sload', 'sstore', 'sfp', 'smuldiv'):
            lat = {'sload': self.cfg.scalar_smu_latency, 'sstore': 1,
                   'sfp': self.cfg.scalar_fpu_latency, 'smuldiv': self.cfg.scalar_muldiv_latency}[k]
            if ins.dst and ins.dst.startswith('vs'):
                self.scoreboard[ins.dst] = t + lat
            self.pc += 1
            note('issue')
            self.stats.instrs_issued += 1
            return
        # 向量指令：需要序列器槽位
        if self.slots_used + ins.slots > self.cfg.n_seq_entries:
            note('seq_full')
            return
        op = self._issue_vector(ins, t)
        self.stats.instrs_issued += 1
        self.stats.vector_ops += 1
        if k == 'branch':
            self.pending_branch = (op, ins)
        elif k == 'rfirst' and ins.dst:
            self.scoreboard[ins.dst] = op
        if k != 'branch':
            self.pc += 1
        note('issue')

    def _maybe_ack(self, blk: Block) -> None:
        if blk.stopped and blk.pending_ops == 0 and not blk.acked:
            blk.acked = True
            if self.vru is not None:
                self.vru.ack_block()

    # ---- 向量发射 ----
    def _issue_vector(self, ins: Instr, t: int) -> VectorOp:
        cfg = self.cfg
        vc = self.kernel.vcfg
        # 速率（混合精度）
        rate = 1
        if cfg.conf_prec:
            regs = [r for r in list(ins.reads) + list(ins.writes) if r.startswith('vv')]
            if regs:
                rate = min(rate_of(cfg, vc.region(int(r[2:]))) for r in regs)
            elif ins.prec:
                rate = rate_of(cfg, ins.prec)
        U = cfg.n_strip
        E = U * rate
        vl = self.vl
        op = VectorOp(self.next_op_id, ins, self.cur_block, vl, rate, E)
        self.next_op_id += 1
        op.issue_time = t
        # 依赖
        for A in self.inflight:
            if A.complete:
                continue
            kinds = set()
            if A.writes & op.reads:
                kinds.add('raw')
            if A.reads & op.writes:
                kinds.add('war')
            if A.writes & op.writes:
                kinds.add('waw')
            if kinds:
                op.deps.append((A, kinds))
        # 元素分配到 lane
        total_chunks = math.ceil(vl / U)
        per_lane: list[list[tuple[int, int]]] = [[] for _ in range(cfg.n_lanes)]
        for c in range(total_chunks):
            per_lane[c % cfg.n_lanes].append((c * U, min((c + 1) * U, vl)))
        if ins.is_mem:
            self._setup_mem(op, ins)
        for l, lane in enumerate(self.lanes):
            lo = LaneOp(op, l, per_lane[l])
            op.lanes.append(lo)
            if ins.is_mem:
                lo.vmu_start = t + cfg.vmu_issue_latency
                lo.beats = [self._beats(op, lo, k) for k in range(lo.nstrips)]
                lo.completions = [[] for _ in range(lo.nstrips)]
                if lo.nstrips:
                    lane.vmu_queue.append(lo)
            lane.ops.append(lo)
        self.inflight.append(op)
        self.slots_used += op.slots
        self.cur_block.pending_ops += 1
        self.stats.mem_elems += vl if ins.is_mem else 0
        return op

    def _setup_mem(self, op: VectorOp, ins: Instr) -> None:
        base_reg, stride_reg = self.mem_regs(ins)
        if ins.mode in ('unit', 'stride') and base_reg and base_reg.startswith('va'):
            op.base = self.va.get(base_reg, 0)
            op.stride_bytes = self.va.get(stride_reg, ins.elsize) if stride_reg else ins.elsize * (ins.seglen + 1)
        elif ins.mode in ('unit', 'stride'):
            op.base = 0x9000_0000
            op.stride_bytes = ins.elsize * (ins.seglen + 1)
            self.stats.warnings.append(f'{ins.text}: base {base_reg} not tracked; assuming fixed region')
        else:
            g = ins.annot.get('gather', '')
            parts = g.split()
            arr = self.kernel.arrays.get(parts[0]) if parts else None
            pattern = parts[1] if len(parts) > 1 else 'random'
            op.gather = (arr, pattern)

    def _beats(self, op: VectorOp, lo: LaneOp, k: int) -> list[int]:
        ins = op.ins
        tb = self.cfg.tl_data_bytes
        out: list[int] = []
        if ins.mode == 'unit':
            step = op.stride_bytes
            for e0, e1 in lo.strip_elems(k):
                lo_b = (op.base + e0 * step) // tb
                hi_b = (op.base + e1 * step + tb - 1) // tb
                out.extend(range(lo_b, hi_b))
            return out
        if ins.mode == 'stride':
            step = op.stride_bytes
            last = -1
            for e0, e1 in lo.strip_elems(k):
                for e in range(e0, e1):
                    for j in range(ins.seglen + 1):
                        b = (op.base + e * step + j * ins.elsize) // tb
                        if b != last:
                            out.append(b)
                            last = b
            return out
        # indexed / AMO
        arr, pattern = op.gather if op.gather else (None, 'random')
        rng = random.Random(op.seed * 1000003 + lo.lane * 7919 + k)
        last = -1
        for e0, e1 in lo.strip_elems(k):
            for e in range(e0, e1):
                if arr is None:
                    a = 0xA000_0000 + rng.randrange(1 << 20) * ins.elsize
                elif pattern == 'unit':
                    a = arr.base + (e % arr.n) * ins.elsize
                else:
                    a = arr.base + rng.randrange(arr.n) * ins.elsize
                b = a // tb
                if b != last:
                    out.append(b)
                    last = b
        return out

    # ---- 主序列器退休 ----
    def retire_step(self, t: int) -> None:
        for op in self.inflight:
            if op.complete:
                continue
            if all(lo.finished and (lo.finish_time is None or lo.finish_time <= t) for lo in op.lanes):
                op.complete_time = t
                self.slots_used -= op.slots
                op.block.pending_ops -= 1
                self._maybe_ack(op.block)
        self.inflight = [o for o in self.inflight if not o.complete]
        for lane in self.lanes:
            lane.retire_step(t)

    # ---- 主循环 ----
    def run(self, max_cycles: int = 200_000_000) -> SimStats:
        cfg = self.cfg
        st = self.stats
        st.n_elems = self.n
        st.iterations = self.iters_total
        st.fu_busy['_n_fma'] = cfg.n_fma_units
        t = 0
        occ_sum = 0
        while True:
            for lane in self.lanes:
                lane.mem_completion_step(t)
            self.retire_step(t)
            for lane in self.lanes:
                lane.vmu_step(t)
                r = lane.vmu_reason
                st.vmu_cycle[r] = st.vmu_cycle.get(r, 0) + 1
                issued = lane.schedule(t)
                key = 'issue' if issued else lane.last_reason
                st.lane_cycle[key] = st.lane_cycle.get(key, 0) + 1
            self.scalar_step(t)
            self.ctrl_step(t)
            if self.vru is not None:
                self.vru.step(t)
            occ_sum += self.slots_used
            if (t & 1023) == 0:
                for lane in self.lanes:
                    lane.prune(t)
            t += 1
            if self.ctrl_done and not self.ctrl_cmds and not self.vcmdq and not self.vf_active \
                    and not self.inflight and self.pending_branch is None:
                break
            if t >= max_cycles:
                st.warnings.append('max_cycles reached')
                break
        st.cycles = t
        st.seq_occupancy = occ_sum / max(1, t)
        st.mem = self.mem.stats
        if self.vru is not None:
            st.vru = dict(self.vru.stats)
            st.vru['runahead_bytes_end'] = self.vru.runahead_bytes
        return st
