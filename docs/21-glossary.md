# 21 术语与缩写表

| 术语 | 含义 |
|---|---|
| ABox0/1/2 | VMU 地址通路的三级：地址生成与翻译 / 合并 / 元数据生成 |
| AGU | address generation unit，VMU 内计算基址（多 lane 时作短迭代乘法器） |
| AIB | atomic instruction block，Scale 的原子指令块 |
| AMO | atomic memory operation，原子访存 |
| AVL / HVL | application vector length / hardware vector length，应用向量长度 / 硬件向量长度 |
| BPQ / BRQ / BWQ | bank predicate queue / bank read queue / bank write queue，bank 级谓词读 / 操作数读 / 写回队列 |
| chaining | 向量链接：后续指令在前一指令的元素完成时立即消费，无需等整条向量 |
| CMDQ / VCMDQ / VRCMDQ | 命令队列 / 向量命令队列（给标量单元）/ 向量预取命令队列（给 VRU） |
| consensual branch | 一致性分支，谓词全真 (all) 或任一真 (any) 时跳转 |
| control thread / worker thread | 控制线程（Rocket 上）/ 工作线程（Hwacha 上） |
| DAE | decoupled access/execute，访存/执行解耦 |
| DCC | decoupled cluster，解耦簇（VGU、VPU、VSU、VLU、VDU） |
| density-time skipping | 跳过连续的假谓词元素以节省时间 |
| divergence | 分歧：SPMD 线程在分支处走向不同路径 |
| eidx | element index，序列器中当前元素索引 |
| EOS / Raven / Hurricane | Berkeley 三个系列的测试芯片 |
| esp-tools | Hwacha 专用的 riscv-tools 分支 |
| expander | 展开器，把序列器操作展开成 bank µop |
| FF RF | flip-flop register file，bank 内小型触发器寄存器堆（当前未启用） |
| FPREQQ / FPRESPQ | 标量单元到 Rocket FPU 的请求/应答队列 |
| gather / scatter | 索引 load / 索引 store |
| HOV / MXP | high-occupancy vector lanes / mixed precision，混合精度扩展的两个名称 |
| IBox | VMU issue box，接收访存命令并拆分为连续段 |
| lane | 向量通道，含 VXU 与 VMU |
| lane stride / lstrip | 多 lane 元素交错粒度 |
| LPQ / LRQ | lane predicate queue / lane read queue，供共享变延迟单元与 VGU 的谓词/操作数队列 |
| MBox | VMU memory box，维护在途请求表并发出 TileLink 请求 |
| MOU | memory ordering unit，内存排序单元 |
| MRT | memory request tracker（MemTracker），在途访存计数 |
| OPL / PDL | operand latch / predicate latch，操作数/谓词锁存器 |
| PBox0/1 | VMU 谓词通路：地址掩码 / store 字节掩码 |
| PLU | predicate logic unit，谓词逻辑单元 |
| PRF / VRF | predicate register file / vector register file |
| PTW | page table walker |
| RoCC | Rocket Custom Coprocessor 接口 |
| RPred / RFirst | 谓词归约（all/any）/ vfirst 归约 |
| RVV | RISC-V 标准向量扩展（Hwacha 不实现） |
| SBox | VMU store 对齐单元 |
| scalarization | 标量化：把线程一致的值放进标量寄存器 |
| scoreboard | 标量单元中跟踪长延迟写目的寄存器的位图 |
| sequencer | 序列器，向量指令发射窗口（master + per-lane） |
| slice | bank 内一个 64 位数据通路，每 bank 2 个 |
| SMU | scalar memory unit，标量访存单元 |
| SPMD | single program multiple data |
| stripmine | 分条循环：把长向量按 HVL 分段处理 |
| strip | 8 个 64 位元素，一次穿过全部 bank 的工作量，序列器的发射粒度 |
| systolic bank execution | µop 逐 bank、逐周期传递的无停顿执行方式 |
| SXU | scalar execution unit，标量单元 |
| TBox | VMU/SMU 的 TLB 接口盒 |
| ticker | 展开器内的 µop 移位寄存器 |
| TileLink | Rocket Chip 的片上互连与一致性协议 |
| VCU | VMU 地址翻译簿记序列器操作 |
| VDU | vector decoupled unit，变延迟功能单元簇（IDiv、FDiv、归约） |
| vector-fetch block | 向量取指块，`vf` 指向、`vstop` 结束的工作线程代码 |
| VFU0/1/2 | 共享向量功能单元簇 |
| VGU / VPU / VSU / VLU | 向量地址生成 / 谓词 / store 数据 / load 数据单元 |
| VIU / VIMU / VIPU / VFMU / VFCU / VFVU / VIDU / VFDU / VQU | 序列器操作类型：整数 / 整数乘 / 谓词 / FMA / 浮点比较 / 浮点转换 / 整数除簿记 / 浮点除簿记 / 变延迟单元读出 |
| VI$ | vector instruction cache，向量指令缓存 |
| VLDQ / VSDQ / VVAQ / VPAQ / VPQ | load 数据 / store 数据 / 虚地址 / 物理地址 / 谓词队列 |
| VMT | vector memory table，VMU 在途请求表 |
| VMU / VXU | vector memory unit / vector execution unit |
| VRU | vector runahead unit，向量预取单元 |
| vs / va / vv / vp | 向量共享寄存器 / 向量地址寄存器 / 向量数据寄存器 / 向量谓词寄存器 |
| vsetcfg / vsetvl / vmcs / vmca / vf | 控制线程指令：配置 / 设向量长度 / 移到共享寄存器 / 移到地址寄存器 / 向量取指 |
| XHwacha | Chipyard 对 Hwacha 非标准扩展的命名 |
| µop (bank micro-op) | bank 级微操作，systolic 数据通路的执行单位 |
