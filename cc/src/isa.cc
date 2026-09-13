#include "isa.hh"
#include "sim.hh"
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace hw {

const char *kindName(Kind k) {
    static const char *n[] = {"load", "store", "amo", "pmem", "alu", "imul", "idiv", "fma", "fdiv", "fconv", "fcmp",
                              "cmp", "plu", "rfirst", "scalar", "sload", "sstore", "sfp", "smuldiv", "stop", "fence", "branch"};
    return n[(int)k];
}

static const std::regex REG_RE(R"(^(vv|vs|va|vp)(\d+)$)");
static const std::regex FMA_RE(R"(^vf(add|sub|mul|madd|msub|nmsub|nmadd)\.([dsh])(?:\.([dsh]))?$)");
static const std::regex FDIV_RE(R"(^vf(div|sqrt)\.([dsh])$)");
static const std::regex FCVT_RE(R"(^vfcvt\.([dshwl]u?)\.([dshwl]u?)$)");
static const std::regex FCMP_RE(R"(^vf(sgnj|sgnjn|sgnjx|min|max|class)\.([dsh])$)");
static const std::regex VCMPF_RE(R"(^vcmpf(eq|lt|le)\.([dsh])$)");
static const std::regex VCMPI_RE(R"(^vcmp(eq|lt|ltu|ez|nez)$)");
static const std::regex VMEM_RE(R"(^v(l|s)(seg)?(st|x)?(b|h|w|d)(u)?$)");
static const std::regex SMEM_RE(R"(^v(l|s)(a|s)(b|h|w|d)(u)?$)");
static const std::regex AMO_RE(R"(^vamo(swap|add|and|or|xor|min|max|minu|maxu)\.([wd])$)");
static const std::regex PLU_RE(R"(^vp(op|clear|set|(?:xor|or|and)(?:xor|or|and))$)");
static const std::regex BR_RE(R"(^vcjal(r)?(?:\.(all|any))?$)");
static const std::set<std::string> ALU_SET = {"vadd", "vaddu", "vsub", "vsll", "vsrl", "vsra", "vand", "vor", "vxor",
    "vslt", "vsltu", "veidx", "vaddw", "vsubw", "vsllw", "vsrlw", "vsraw"};
static const std::set<std::string> IMUL_SET = {"vmul", "vmulh", "vmulhu", "vmulhsu", "vmulw"};
static const std::set<std::string> IDIV_SET = {"vdiv", "vdivu", "vrem", "vremu", "vdivw", "vdivuw", "vremw", "vremuw"};
static const std::set<std::string> SCALAR_IMM = {"vaddi", "vslli", "vsrli", "vsrai", "vandi", "vori", "vxori", "vslti",
    "vsltiu", "vaddiw", "vslliw", "vsrliw", "vsraiw", "vlui", "vauipc"};

static unsigned widthBytes(char c) { return c == 'b' ? 1 : c == 'h' ? 2 : c == 'w' ? 4 : 8; }
static unsigned slotsOf(Kind k) {
    switch (k) {
        case Kind::Load: case Kind::Store: case Kind::PMem: return 3;
        case Kind::Amo: return 4;
        case Kind::IDiv: case Kind::FDiv: return 2;
        case Kind::Alu: case Kind::IMul: case Kind::Fma: case Kind::FConv: case Kind::FCmp: case Kind::Cmp:
        case Kind::Plu: case Kind::RFirst: case Kind::Branch: return 1;
        default: return 0;
    }
}

bool isReg(const std::string &s) { return std::regex_match(s, REG_RE); }
int regIndex(const std::string &s) { return std::stoi(s.substr(2)); }
static bool isVReg(const std::string &s) { return s.rfind("vv", 0) == 0; }

std::set<std::string> Instr::reads() const {
    std::set<std::string> r;
    for (auto &s : srcs) if (isReg(s)) r.insert(s);
    if (!pred.empty()) r.insert(pred);
    return r;
}
std::set<std::string> Instr::writes() const {
    std::set<std::string> w;
    if (!dst.empty() && isReg(dst)) { if (writesVrf()) for (auto &d : dstRegs()) w.insert(d); else w.insert(dst); }
    return w;
}
std::vector<std::string> Instr::dstRegs() const {
    if (dst.empty()) return {};
    if (!writesVrf()) return {dst};
    std::vector<std::string> v;
    int base = regIndex(dst);
    for (unsigned i = 0; i <= seglen; ++i) v.push_back("vv" + std::to_string(base + i));
    return v;
}
std::pair<std::string, std::string> Instr::memRegs() const {
    std::vector<std::string> s = (kind == Kind::Load || (kind == Kind::PMem && mnemonic == "vpl")) ? srcs
                                 : std::vector<std::string>(srcs.begin() + (srcs.empty() ? 0 : 1), srcs.end());
    std::string base = s.empty() ? "" : s[0];
    std::string stride = (mode == Mode::Stride && s.size() > 1) ? s[1] : "";
    return {base, stride};
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string cleanOperand(std::string t) {
    t = trim(t);
    if (!t.empty() && t.back() == ',') t.pop_back();
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = t.substr(1, t.size() - 2);
    return t;
}

static void classify(Instr &ins) {
    const std::string &mn = ins.mnemonic;
    std::vector<std::string> ops = ins.operands;
    std::smatch m;
    if (mn == "vstop") { ins.kind = Kind::Stop; return; }
    if (mn == "vfence") { ins.kind = Kind::Fence; return; }
    if (std::regex_match(mn, m, BR_RE)) {
        ins.kind = Kind::Branch; ins.isVector = true; ins.slots = slotsOf(ins.kind);
        // 两种写法：`vcjal.any sd, label` 与 hwacha-cc 的 `vcjal <cond>, sd, label`（cond 0 = all, 1 = any）
        if (!ops.empty()) {
            size_t first = (!m[2].matched && ops.size() >= 3) ? 1 : 0;
            ins.dst = ops[first]; ins.label = ops.back();
        }
        return;
    }
    if (mn == "vpl" || mn == "vps") {
        ins.kind = Kind::PMem; ins.isVector = true; ins.elsize = 1; ins.mode = Mode::Unit;
        if (mn == "vpl") { ins.dst = ops[0]; ins.srcs = {ops[1]}; } else { ins.srcs = {ops[0], ops[1]}; }
        ins.slots = slotsOf(ins.kind); return;
    }
    if (std::regex_match(mn, m, SMEM_RE)) {
        bool ld = m[1] == "l";
        ins.kind = ld ? Kind::SLoad : Kind::SStore;
        ins.elsize = widthBytes(m[3].str()[0]);
        if (ld) { ins.dst = ops[0]; ins.srcs.assign(ops.begin() + 1, ops.end()); } else ins.srcs = ops;
        return;
    }
    if (std::regex_match(mn, m, VMEM_RE)) {
        bool ld = m[1] == "l";
        ins.kind = ld ? Kind::Load : Kind::Store;
        ins.elsize = widthBytes(m[4].str()[0]);
        ins.mode = m[3] == "st" ? Mode::Stride : m[3] == "x" ? Mode::Indexed : Mode::Unit;
        ins.isVector = true;
        if (m[2].matched) { ins.seglen = std::stoi(ops.back()); ops.pop_back(); }
        if (ld) { ins.dst = ops[0]; ins.srcs.assign(ops.begin() + 1, ops.end()); } else ins.srcs = ops;
        ins.rp = std::count_if(ins.srcs.begin(), ins.srcs.end(), isVReg);
        ins.slots = slotsOf(ins.kind);
        ins.prec = ins.elsize == 8 ? 'd' : ins.elsize == 4 ? 's' : 'h';
        return;
    }
    if (std::regex_match(mn, m, AMO_RE)) {
        ins.kind = Kind::Amo; ins.isVector = true; ins.mode = Mode::Indexed;
        ins.elsize = widthBytes(m[2].str()[0]);
        ins.dst = ops[0]; ins.srcs.assign(ops.begin() + 1, ops.end());
        ins.rp = std::count_if(ins.srcs.begin(), ins.srcs.end(), isVReg);
        ins.slots = slotsOf(ins.kind); ins.prec = ins.elsize == 8 ? 'd' : 's';
        return;
    }
    if (std::regex_match(mn, m, FMA_RE)) { ins.kind = Kind::Fma; ins.prec = m[2].str()[0]; }
    else if (std::regex_match(mn, m, FDIV_RE)) { ins.kind = Kind::FDiv; ins.prec = m[2].str()[0]; }
    else if (std::regex_match(mn, m, FCVT_RE)) { ins.kind = Kind::FConv; char a = m[1].str()[0]; ins.prec = (a == 'd' || a == 's' || a == 'h') ? a : 'd'; }
    else if (std::regex_match(mn, m, FCMP_RE)) { ins.kind = Kind::FCmp; ins.prec = m[2].str()[0]; }
    else if (std::regex_match(mn, m, VCMPF_RE)) { ins.kind = Kind::FCmp; ins.prec = m[2].str()[0]; }
    else if (std::regex_match(mn, m, VCMPI_RE)) { ins.kind = Kind::Cmp; ins.prec = 'd'; }
    else if (ALU_SET.count(mn)) { ins.kind = Kind::Alu; ins.prec = mn.back() == 'w' ? 's' : 'd'; }
    else if (IMUL_SET.count(mn)) { ins.kind = Kind::IMul; ins.prec = mn.back() == 'w' ? 's' : 'd'; }
    else if (IDIV_SET.count(mn)) { ins.kind = Kind::IDiv; ins.prec = mn.back() == 'w' ? 's' : 'd'; }
    else if (std::regex_match(mn, PLU_RE)) { ins.kind = Kind::Plu; ins.isVector = true; }
    else if (mn == "vfirst") { ins.kind = Kind::RFirst; ins.isVector = true; }
    else if (SCALAR_IMM.count(mn)) { ins.kind = Kind::Scalar; }
    else throw std::runtime_error("line " + std::to_string(ins.lineNo) + ": unknown mnemonic '" + mn + "'");

    if (!ops.empty()) { ins.dst = ops[0]; ins.srcs.assign(ops.begin() + 1, ops.end()); }
    if (ins.kind == Kind::Plu) { std::vector<std::string> s; for (auto &x : ins.srcs) if (isReg(x)) s.push_back(x); ins.srcs = s; }
    if (mn == "vcmpez" || mn == "vcmpnez") ins.srcs.push_back("vs0");
    if (ins.kind == Kind::Scalar) return;
    bool vec = std::any_of(ops.begin(), ops.end(), isVReg) || ins.kind == Kind::Plu || ins.kind == Kind::RFirst;
    ins.isVector = vec;
    if (!vec) {
        if (ins.kind == Kind::Fma || ins.kind == Kind::FDiv || ins.kind == Kind::FConv || ins.kind == Kind::FCmp) ins.kind = Kind::SFp;
        else if (ins.kind == Kind::IMul || ins.kind == Kind::IDiv) ins.kind = Kind::SMulDiv;
        else ins.kind = Kind::Scalar;
        return;
    }
    ins.rp = std::count_if(ins.srcs.begin(), ins.srcs.end(), isVReg);
    ins.slots = slotsOf(ins.kind);
}

bool parseInstr(const std::string &line, int lineNo, Instr &ins) {
    ins = Instr();
    ins.lineNo = lineNo;
    std::string code = line, comment;
    size_t hash = line.find('#');
    if (hash != std::string::npos) { code = line.substr(0, hash); comment = line.substr(hash + 1); }
    static const std::regex ANN(R"(@(\w+)(?:\s+([^@]+))?)");
    for (auto it = std::sregex_iterator(comment.begin(), comment.end(), ANN); it != std::sregex_iterator(); ++it)
        ins.annot[(*it)[1]] = trim((*it)[2].matched ? (*it)[2].str() : "");
    code = trim(code);
    if (code.empty() || code.back() == ':') return false;
    std::string flat = code;
    std::replace(flat.begin(), flat.end(), ',', ' ');
    std::istringstream is(flat);
    std::vector<std::string> toks;
    for (std::string t; is >> t;) toks.push_back(t);
    while (!toks.empty()) {
        const std::string &t = toks.front();
        bool predTok = t[0] == '@' || t[0] == '!' ||
                       (t.rfind("vp", 0) == 0 && isReg(t) && toks.size() > 1 && !isReg(toks[1]) && !SCALAR_IMM.count(toks[1]));
        if (!predTok) break;
        std::string p = t; toks.erase(toks.begin());
        if (p == "@all" || p == "@s") continue;
        ins.predNeg = p.rfind("@!", 0) == 0 || p[0] == '!';
        size_t k = 0; while (k < p.size() && (p[k] == '@' || p[k] == '!')) ++k;
        ins.pred = p.substr(k);
        if (!isReg(ins.pred) || ins.pred.rfind("vp", 0) != 0) throw std::runtime_error("line " + std::to_string(lineNo) + ": bad predicate " + p);
    }
    if (toks.empty()) return false;
    ins.mnemonic = toks[0];
    std::transform(ins.mnemonic.begin(), ins.mnemonic.end(), ins.mnemonic.begin(), ::tolower);
    for (size_t i = 1; i < toks.size(); ++i) ins.operands.push_back(cleanOperand(toks[i]));
    ins.text = code;
    classify(ins);
    return true;
}

// ------------------------------------------------------------------ Kernel
uint64_t Kernel::vaStrideBytes(const std::string &reg) const {
    auto it = va.find(reg);
    if (it == va.end()) return 8;
    if (!it->second.isPtr) return it->second.value;
    return arrays.at(it->second.array).elem * it->second.strideElems;
}

static std::map<std::string, std::string> kvPairs(const std::string &s) {
    std::map<std::string, std::string> kv;
    static const std::regex KV(R"((\w+)=([\w.]+))");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), KV); it != std::sregex_iterator(); ++it) kv[(*it)[1]] = (*it)[2];
    return kv;
}

Kernel parseKernel(const std::string &text, const std::string &path) {
    Kernel k; k.path = path;
    uint64_t nextBase = 0x80000000ull;
    std::istringstream is(text);
    std::string raw; int ln = 0;
    while (std::getline(is, raw)) {
        ++ln;
        std::string line = trim(raw);
        if (line.rfind("#", 0) == 0) {
            std::string body = trim(line.substr(1));
            if (body.rfind("@", 0) != 0) continue;
            std::istringstream bs(body.substr(1));
            std::string key; bs >> key;
            std::string arg; std::getline(bs, arg); arg = trim(arg);
            if (key == "kernel") k.name = arg;
            else if (key == "n") k.n = std::stoull(arg, nullptr, 0);
            else if (key == "cfg") {
                auto kv = kvPairs(arg);
                k.vcfg.v64 = kv.count("v64") ? std::stoi(kv["v64"]) : 0;
                k.vcfg.v32 = kv.count("v32") ? std::stoi(kv["v32"]) : 0;
                k.vcfg.v16 = kv.count("v16") ? std::stoi(kv["v16"]) : 0;
                k.vcfg.vp = kv.count("vp") ? std::stoi(kv["vp"]) : 0;
            } else if (key == "array") {
                std::istringstream as(arg); ArrayDef a; as >> a.name;
                std::string rest; std::getline(as, rest);
                auto kv = kvPairs(rest);
                a.elem = kv.count("elem") ? std::stoi(kv["elem"]) : 8;
                a.n = kv.count("n") ? std::stoull(kv["n"], nullptr, 0) : k.n;
                if (kv.count("base")) a.base = std::stoull(kv["base"], nullptr, 0);
                else { a.base = nextBase; nextBase = (nextBase + a.nbytes() + 0xFFFF) & ~0xFFFull; }
                k.arrays[a.name] = a;
            } else if (key == "va") {
                static const std::regex VA(R"((va\d+)\s*=\s*(\S+)(.*))");
                std::smatch m;
                if (!std::regex_match(arg, m, VA)) throw std::runtime_error("line " + std::to_string(ln) + ": bad @va");
                VaDef d; d.reg = m[1]; std::string target = m[2], rest = m[3];
                if (k.arrays.count(target)) {
                    auto kv = kvPairs(rest);
                    d.isPtr = true; d.array = target;
                    d.strideElems = kv.count("stride") ? std::stoi(kv["stride"]) : 1;
                    d.offsetElems = kv.count("offset") ? std::stoi(kv["offset"]) : 0;
                    d.advance = rest.find("fixed") == std::string::npos;
                } else { d.isPtr = false; d.value = std::stoull(target, nullptr, 0); }
                if (!k.va.count(d.reg)) k.vaOrder.push_back(d.reg);
                k.va[d.reg] = d;
            } else if (key == "vs") k.nVmcs = std::stoi(arg);
            else if (key == "ctrl") k.ctrlCycles = std::stoi(arg);
            else if (key == "iters") k.iters = std::stoi(arg);
            else if (key == "keep") {}
            else throw std::runtime_error("line " + std::to_string(ln) + ": unknown directive @" + key);
            continue;
        }
        if (line.empty()) continue;
        static const std::regex LBL(R"(^(\.?[A-Za-z_]\w*):\s*(.*)$)");
        std::smatch m;
        if (std::regex_match(line, m, LBL)) { k.labels[m[1]] = (int)k.instrs.size(); line = m[2]; if (line.empty()) continue; }
        if (line[0] == '.') continue;   // 汇编伪指令（.align 等）
        Instr ins;
        if (parseInstr(line, ln, ins)) k.instrs.push_back(ins);
    }
    if (std::none_of(k.instrs.begin(), k.instrs.end(), [](const Instr &i) { return i.kind == Kind::Stop; }))
        throw std::runtime_error("vector-fetch block must end with vstop");
    for (auto &ins : k.instrs) {
        if (ins.kind == Kind::Branch && !k.labels.count(ins.label))
            throw std::runtime_error("line " + std::to_string(ins.lineNo) + ": unknown label " + ins.label);
        if (ins.isMem() && ins.mode != Mode::Indexed) {
            auto [base, stride] = ins.memRegs();
            if (base.rfind("va", 0) == 0 && !k.va.count(base))
                throw std::runtime_error("line " + std::to_string(ins.lineNo) + ": " + base + " not described by @va");
        }
    }
    return k;
}

Kernel loadKernel(const std::string &path) {
    std::ifstream f(path);
    if (!f) sim::fatal("cannot open kernel " + path);
    std::stringstream ss; ss << f.rdbuf();
    return parseKernel(ss.str(), path);
}

}  // namespace hw
