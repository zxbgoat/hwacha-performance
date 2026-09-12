# 03 ISA 参考

Hwacha ISA 是 RISC-V 的非标准扩展（Chipyard 中称 XHwacha），是一个向量 load-store 架构。本文按控制线程指令、工作线程指令格式、工作线程指令类别、异常四部分整理。完整的位级编码请以架构手册（EECS-2015-262）第 7 节和 `ucb-bar/esp-opcodes` 的 `opcodes-hwacha` 为准。

## 1. 控制线程指令

32 位，标准 RoCC 格式：

```
31      25 24  20 19  15 14  13  12  11   7 6      0
 funct7    rs2    rs1   xd  xs1 xs2   rd    opcode
```

| 指令 | opcode | 含义 |
|---|---|---|
| `vsetcfg rs1, imm[11:0]` | custom-0 | 用 rs1 高 52 位 + 12 位立即数拼成 64 位 vcfg，重划分寄存器堆，计算最大 HVL，并将 vlen 清零 |
| `vsetvl rd, rs1` | custom-0 | vlen = min(rs1, HVL)，写回 rd |
| `vgetcfg rd` | custom-0 | 读 vcfg |
| `vgetvl rd` | custom-0 | 读 vlen |
| `vuncfg` | custom-0 | 清除配置，向量单元回到未配置状态 |
| `vmcs srd, rs1` | custom-1 | rs1 → 共享寄存器 vs[srd]（srd 分高低位编码在 rs2 和 rd 字段） |
| `vmca ard, rs1` | custom-1 | rs1 → 地址寄存器 va[ard] |
| `vf rs1, imm[11:0]` | custom-1 | 从 rs1 + imm 处开始执行向量取指块 |

**vcfg 布局**

```
63        32 31   23 22   14 13  9 8    0
     0        #v16    #v32   #pred  #v64
```

汇编器接受 `vsetcfg #v64, #pred` 或 `vsetcfg #v64, #pred, #v32, #v16`，并自动生成构造 64 位常量的指令序列。#v* 取值 0–256，#pred 取值 0–16。

**未配置状态**：复位后或 `vuncfg` 之后，除 `vsetcfg` 外的任何控制线程指令都会触发 accelerator disabled 异常。

## 2. 工作线程指令格式

所有工作线程指令 64 位定长，必须 8 字节对齐，低 7 位固定为 `0111111`，剩余 5 位主操作码大致沿用 RISC-V 的主操作码布局（LOAD、STORE、OP、OP-FP、MADD、AMO、CTRL 等）。

```
63 62 61 60 59   53 52  50 49    41 40 35 34 33 32 31   24 23  16 15 12 11    0
                 imm[31:3]                 c2    n   rs1     rd     p    opcode   VJ
                 imm[31:0]                funct8        rd   funct4  opcode   VU
                 imm[31:0]                 rs1          rd   funct4  opcode   VI
 d  1  2  f funct7 funct3  funct9   rs2    n   rs1     rd     p    opcode   VR
 d  1  2  3 funct7 funct3 f  rs3    rs2    n   rs1     rd     p    opcode   VR4
```

字段语义：

- **d / 1 / 2 / 3 标志（bit 63–60）**：分别指示 rd、rs1、rs2、rs3 是向量数据寄存器（置 1）还是共享寄存器（清 0）。全部为共享寄存器的指令是**标量指令**（汇编前缀 `@s`），只执行一次；任一为向量寄存器则按 vlen 执行。某些指令用这些字段索引地址寄存器（as1、as2）。地址寄存器永远不能作为目的。
- **p（bit 15–12）**：谓词寄存器号。**n（bit 32）**：谓词取反。不带谓词的指令汇编前缀为 `@all`。
- rs1、rs2、rd、p 在所有格式中位置固定，简化译码；立即数左对齐。

标量指令的 p 字段必须为 0 且 n 必须清零，否则非法指令异常。

汇编书写约定：`vd`/`vs1`/`vs2`/`vs3` 表示向量数据寄存器，`sd`/`ss1`/`ss2`/`ss3` 表示共享寄存器，`as1`/`as2` 表示地址寄存器，`pd`/`ps*` 表示谓词寄存器，`rd`/`rs*` 表示可以是 v 或 s 二者之一。

## 3. 工作线程指令类别

### 3.1 单位步长 / 常量步长 / 分段访存

```
@[!]p vl{b,h,w,d,bu,hu,wu}      vd, as1
@[!]p vlst{b,h,w,d,bu,hu,wu}    vd, as1, as2
@[!]p vlseg{b,h,w,d,bu,hu,wu}   vd, as1, seglen
@[!]p vlsegst{b,h,w,d,bu,hu,wu} vd, as1, as2, seglen
@[!]p vs{b,h,w,d}               vd, as1
@[!]p vsst{b,h,w,d}             vd, as1, as2
@[!]p vsseg{b,h,w,d}            vd, as1, seglen
@[!]p vssegst{b,h,w,d}          vd, as1, as2, seglen
```

- 基址来自 as1，常量步长来自 as2。
- `seglen` 字段（3 位）表示连续 seglen+1 个向量寄存器参与传输，用于 AoS 布局；非分段访存是 seglen=0 的退化情形。
- 语义（加载）：

```c
stride = unit ? elsize : areg[as2];
for i in 0..vl-1:
  if ([!]preg[p][i])
    for j in 0..seglen:
      vreg[vd+j][i] = mem[areg[as1] + (i*(seglen+1)+j)*stride];
```

- 支持 4 种宽度，加载有符号/零扩展两种。

### 3.2 索引访存（gather / scatter）

```
@[!]p vlx{b,h,w,d,bu,hu,wu}    vd, ss1, vs2
@[!]p vlsegx{...}              vd, ss1, vs2, seglen
@[!]p vsx{b,h,w,d}             vd, ss1, vs2
@[!]p vssegx{...}              vd, ss1, vs2, seglen
```

基址来自共享寄存器 ss1，偏移来自向量寄存器 vs2（符号扩展）。有效地址 = ss1 + vs2[i] + j×elsize。

### 3.3 向量原子访存

```
@[!]p vamo{swap,add,and,or,xor,min,max,minu,maxu}.{w,d} vd, (rs1), rs2
```

rs1（地址）与 rs2（数据）都可以是 v 或 s。若 rs1 是 ss1，则对同一地址做 vlen 次原子操作；若 rs2 是 ss2，则每次用同一数据。结果永远写入 vd。支持 aq/rl 位，语义与 RISC-V A 扩展一致。原子操作由 L2 cache bank 内的 ALU 执行。

### 3.4 向量整数计算

`veidx vd, rs1`（返回元素索引左移 rs1 位）；`vadd vsub vsll vsrl vsra vand vor vxor vslt vsltu vmul vmulh vmulhu vmulhsu vdiv vdivu vrem vremu` 及 32 位 `*w` 变体。语义与 RV64IM 相同。目的为共享寄存器而源含向量寄存器会触发非法指令异常。

### 3.5 向量归约

`@[!]p vfirst sd, vs1`：返回谓词下第一个活跃元素的值，谓词全空则返回 0。与比较指令配合可以遍历向量中所有不同的值。这是 ISA 中唯一的归约指令；求和等归约通常用 AMO 到 L2 或在标量端完成。

### 3.6 向量浮点计算

`vfadd vfsub vfmul vfdiv vfsqrt vfmadd vfmsub vfnmsub vfnmadd vfsgnj vfsgnjn vfsgnjx vfmin vfmax vfclass`，后缀 `.d/.s/.h`；还有加宽形式 `vfadd.s.h`、`vfmadd.d.h`、`vfmul.d.s` 等。FMA 类使用 VR4 格式。可在最后一个操作数指定静态舍入模式，否则用继承自控制线程的动态舍入模式。整数与浮点共用 vv 寄存器，因此没有 fmv 类指令。

转换：`vfcvt.{d,s,h}.{d,s,h}`、`vfcvt.{w,wu,l,lu}.{d,s,h}`、`vfcvt.{d,s,h}.{w,wu,l,lu}`。

### 3.7 向量比较

```
@[!]p vcmpeq  pd, rs1, rs2
@[!]p vcmplt  pd, rs1, rs2
@[!]p vcmpltu pd, rs1, rs2
@[!]p vcmpf{eq,lt,le}.{d,s,h} pd, rs1, rs2
```

只提供一半的比较条件，反条件通过消费者指令的 n 标志实现。`vcmpez` 等为伪指令。

### 3.8 谓词访存与谓词计算

```
@all vpl pd, as1        # 每字节最低位 → 谓词
@all vps pd, as1        # 谓词 → 符号扩展字节
@all vpop pd, ps1, ps2, ps3, tt   # 8 位真值表定义的三输入逻辑
@all vpclear pd / vpset pd
@all vp{xor,or,and}{xor,or,and} pd, ps1, ps2, ps3   # 伪指令
```

谓词指令永远在全掩码下执行。

### 3.9 标量访存

```
@s vla{b,h,w,d,bu,hu,wu} sd, as1      # 基址来自地址寄存器
@s vsa{b,h,w,d}          as1, ss2
@s vls{b,h,w,d,bu,hu,wu} sd, ss1      # 基址来自共享寄存器
@s vss{b,h,w,d}          ss1, ss2
```

由标量访存单元（SMU）直接访问 L2。

### 3.10 标量计算

寄存器形式即所有操作数为 vs 的算术指令；立即数形式：

```
@s vaddi vslti vsltiu vandi vori vxori sd, ss1, imm[31:0]
@s vslli vsrli vsrai sd, ss1, shamt[5:0]
@s vaddiw vslliw vsrliw vsraiw
@s vlui   sd, imm[31:0]      # imm → 高 32 位
@s vauipc sd, imm[31:0]      # (imm<<32) + vpc
```

`vlui` + `vaddi` 可构造任意 64 位整数或浮点常量。

### 3.11 控制流

```
@all  vstop
@all  vfence                          # 编码里含 pred/succ 字段
@[!]p vcjal.{all,any}  sd, imm[31:3]
@[!]p vcjalr.{all,any} sd, ss1, imm[31:3]
```

一致性跳转：当谓词 p（可取反）**全部**为真（`.all`）或**任一**为真（`.any`）时，把 vpc+8 写入 sd 并跳转。立即数是 8 字节的倍数，29 位可覆盖 32 位范围。

## 4. 异常

工作线程只产生两类异常：

- **未对齐**：访存地址未对齐到数据宽度；指令地址未对齐到 8 字节。
- **非法指令**：目的为共享寄存器而源含向量寄存器；寄存器配置精度低于指令要求的精度；引用超出 vsetcfg 配置范围的向量/谓词寄存器（包括分段访存越界）。

地址翻译失败（缺页）通过可重启异常机制交给 OS 处理，见 `modules/15-memory-ordering-exceptions.md`。

## 5. 与 RVV 的主要差别

| | Hwacha | RVV 1.0 |
|---|---|---|
| 向量指令位置 | 独立向量取指块 | 与标量指令混排 |
| 指令长度 | 64 位 | 32 位 |
| 标量操作数 | 独立 vs 寄存器堆（64 个） | 复用 x/f 寄存器 |
| 精度 | 每条指令固定精度（`.d/.s/.h`） | SEW 由 vtype 控制的多态指令 |
| 向量寄存器数 | 最多 256，由 vsetcfg 决定 | 32，LMUL 分组 |
| 谓词 | 16 个 vp，任意指令可取反谓词 | v0 单掩码 |
| 分支 | 块内一致性分支 | 无 |
| 工具链 | esp-tools（专用） | 上游 GCC/LLVM |

架构手册第 4 节提到未来可能采用多态指令集（精度由寄存器配置决定），这一思路在 RVV 中以 SEW/vtype 形式实现。
