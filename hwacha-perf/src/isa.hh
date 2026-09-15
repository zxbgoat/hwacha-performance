// Hwacha 工作线程指令分类与内核文件解析（与 hwacha_perf/isa.py、program.py 同一格式）
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hw {

enum class Kind {
    Load, Store, Amo, PMem, Alu, IMul, IDiv, Fma, FDiv, FConv, FCmp, Cmp, Plu, RFirst,
    Scalar, SLoad, SStore, SFp, SMulDiv, Stop, Fence, Branch
};
enum class Mode { None, Unit, Stride, Indexed };
const char *kindName(Kind k);

struct Instr {
    std::string mnemonic, text;
    std::vector<std::string> operands;
    Kind kind = Kind::Scalar;
    std::string pred;              // "vp3" 或空
    bool predNeg = false;
    char prec = 'd';               // 'd' 's' 'h'
    unsigned elsize = 8;
    Mode mode = Mode::None;
    unsigned seglen = 0;
    std::string dst;
    std::vector<std::string> srcs;
    bool isVector = false;
    unsigned rp = 0;               // 需要 SRAM 读口的向量源操作数个数
    unsigned slots = 0;
    std::map<std::string, std::string> annot;
    std::string label;
    int lineNo = 0;

    bool isMem() const { return kind == Kind::Load || kind == Kind::Store || kind == Kind::Amo || kind == Kind::PMem; }
    bool isLoad() const { return kind == Kind::Load || kind == Kind::Amo || (kind == Kind::PMem && mnemonic == "vpl"); }
    bool isStore() const { return kind == Kind::Store || kind == Kind::Amo || (kind == Kind::PMem && mnemonic == "vps"); }
    bool writesVrf() const { return dst.rfind("vv", 0) == 0; }
    bool writesPrf() const { return dst.rfind("vp", 0) == 0; }
    std::set<std::string> reads() const;
    std::set<std::string> writes() const;
    std::vector<std::string> dstRegs() const;
    // (基址寄存器, 步长寄存器)
    std::pair<std::string, std::string> memRegs() const;
};

bool isReg(const std::string &s);
int regIndex(const std::string &s);
// 解析一行；返回 false 表示空行/仅注释
bool parseInstr(const std::string &line, int lineNo, Instr &out);

struct VCfg { unsigned v64 = 1, v32 = 0, v16 = 0, vp = 0; unsigned nvv() const { return v64 + v32 + v16; }
              char region(unsigned idx) const { return idx < v64 ? 'd' : idx < v64 + v32 ? 's' : 'h'; } };
struct ArrayDef { std::string name; unsigned elem = 8; uint64_t n = 0; uint64_t base = 0; uint64_t nbytes() const { return elem * n; } };
struct VaDef { std::string reg; bool isPtr = true; std::string array; uint64_t value = 0; int strideElems = 1; bool advance = true; int offsetElems = 0; };

struct Kernel {
    std::string name = "kernel", path;
    uint64_t n = 4096;
    VCfg vcfg;
    std::map<std::string, ArrayDef> arrays;
    std::map<std::string, VaDef> va;        // 按寄存器名排序（va0, va1, …）
    std::vector<std::string> vaOrder;
    unsigned nVmcs = 0;
    int ctrlCycles = -1;
    int iters = 0;
    std::vector<Instr> instrs;
    std::map<std::string, int> labels;
    uint64_t vaStrideBytes(const std::string &reg) const;
};

Kernel loadKernel(const std::string &path);
Kernel parseKernel(const std::string &text, const std::string &path = "");

}  // namespace hw
