"""内存系统时序模型：共享 L2（分 bank、组相联、LRU、写分配）、DRAM 通道带宽/延迟、预取。

request() 在请求时刻直接给出完成时刻（基于 bank/通道的"下一空闲时刻"队列模型），
调用方按完成时刻推进。这比逐周期事件模拟粗，但保留了带宽、延迟、bank 冲突与
命中/缺失/预取命中的一阶效应。
"""
from __future__ import annotations
from collections import OrderedDict
from dataclasses import dataclass, field
import heapq
from .config import MemoryConfig


@dataclass
class MemStats:
    reads: int = 0
    writes: int = 0
    hits: int = 0
    misses: int = 0
    pending_hits: int = 0        # 命中在途行（含预取命中）
    prefetch_hits: int = 0       # 由 VRU 预取且随后被需求访问用到的行（每行计一次）
    prefetches: int = 0
    prefetch_useless: int = 0    # 预取时已在 L2 中
    dram_lines: int = 0
    dram_writebacks: int = 0
    bank_queue_cycles: int = 0
    tracker_wait_cycles: int = 0

    @property
    def dram_bytes(self) -> int:
        return 0


class _Set:
    __slots__ = ('lines',)

    def __init__(self):
        self.lines: OrderedDict[int, list] = OrderedDict()   # line -> [dirty, arrival, prefetched]


class MemorySystem:
    def __init__(self, mcfg: MemoryConfig):
        self.c = mcfg
        self.n_sets = mcfg.l2_bytes_per_bank // (mcfg.line_bytes * mcfg.l2_ways)
        self.sets: dict[tuple[int, int], _Set] = {}
        self.bank_free = [0] * mcfg.l2_banks
        self.trackers: list[list[int]] = [[] for _ in range(mcfg.l2_banks)]   # 在途 miss 到达时刻（堆）
        self.dram_free = [0.0] * mcfg.dram_channels
        self.stats = MemStats()
        self.line_cycles = mcfg.line_bytes / mcfg.dram_bytes_per_cycle_per_channel

    # ---- 地址映射 ----
    def _line(self, addr: int) -> int:
        return addr // self.c.line_bytes

    def _bank(self, line: int) -> int:
        return line % self.c.l2_banks

    def _set(self, line: int) -> _Set:
        key = (self._bank(line), (line // self.c.l2_banks) % self.n_sets)
        s = self.sets.get(key)
        if s is None:
            s = self.sets[key] = _Set()
        return s

    def _chan(self, line: int) -> int:
        return (line // 4) % self.c.dram_channels

    # ---- 核心接口 ----
    def request(self, t: int, addr: int, is_store: bool = False, prefetch: bool = False) -> int:
        """发起对 addr 所在 64B 行的一次访问，返回数据/应答可用的时刻。"""
        c, st = self.c, self.stats
        line = self._line(addr)
        bank = self._bank(line)
        # L2 bank 每周期接受一个请求
        service = max(t, self.bank_free[bank])
        self.bank_free[bank] = service + 1
        st.bank_queue_cycles += service - t
        s = self._set(line)
        ent = s.lines.get(line)
        if ent is not None:
            s.lines.move_to_end(line)
            dirty, arrival, pf = ent
            if is_store:
                ent[0] = True
            if prefetch:
                st.prefetch_useless += 1
                return max(service, arrival)
            if is_store:
                st.writes += 1
            else:
                st.reads += 1
            if pf:
                st.prefetch_hits += 1
                ent[2] = False
            if arrival <= service:
                st.hits += 1
                return service + c.l2_hit_latency
            st.pending_hits += 1
            return max(service + c.l2_hit_latency, arrival + 2)
        # miss：tracker 限制
        trk = self.trackers[bank]
        while trk and trk[0] <= service:
            heapq.heappop(trk)
        if len(trk) >= c.l2_trackers_per_bank:
            wait_until = heapq.heappop(trk)
            st.tracker_wait_cycles += max(0, wait_until - service)
            service = max(service, wait_until)
        ch = self._chan(line)
        start = max(float(service), self.dram_free[ch])
        arrival = int(start + c.dram_latency)
        self.dram_free[ch] = start + self.line_cycles
        st.dram_lines += 1
        heapq.heappush(trk, arrival)
        # 写分配 + LRU 替换
        if len(s.lines) >= c.l2_ways:
            _, (vd, _, _) = s.lines.popitem(last=False)
            if vd:
                st.dram_writebacks += 1
                vch = ch  # 近似：写回占用同一通道带宽
                self.dram_free[vch] += self.line_cycles
        s.lines[line] = [bool(is_store), arrival, prefetch]
        if prefetch:
            st.prefetches += 1
            return arrival
        st.misses += 1
        if is_store:
            st.writes += 1
        else:
            st.reads += 1
        return arrival + 2

    def is_present(self, addr: int) -> bool:
        line = self._line(addr)
        return line in self._set(line).lines
