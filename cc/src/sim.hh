// 事件驱动模拟框架（参照 gem5：EventQueue / Event / SimObject / ClockedObject / Stats / Params）
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <stdexcept>

namespace sim {

using Tick = uint64_t;     // 全局时间单位：ps
using Cycles = uint64_t;
constexpr Tick MaxTick = ~Tick(0);

// ---------------------------------------------------------------- Event
class Event {
public:
    enum Priority { Minimum_Pri = -100, Default_Pri = 0, CPU_Tick_Pri = 50, Stat_Pri = 90, Maximum_Pri = 100 };
    explicit Event(int prio = Default_Pri) : _prio(prio) {}
    virtual ~Event() = default;
    virtual void process() = 0;
    virtual std::string name() const { return "Event"; }
    Tick when() const { return _when; }
    bool scheduled() const { return _scheduled; }
    int priority() const { return _prio; }
private:
    friend class EventQueue;
    Tick _when = 0;
    uint64_t _seq = 0;
    int _prio;
    bool _scheduled = false;
    bool _squashed = false;
};

class EventFunctionWrapper : public Event {
public:
    EventFunctionWrapper(std::function<void()> fn, std::string n, int prio = Default_Pri)
        : Event(prio), _fn(std::move(fn)), _name(std::move(n)) {}
    void process() override { _fn(); }
    std::string name() const override { return _name; }
private:
    std::function<void()> _fn;
    std::string _name;
};

// ------------------------------------------------------------ EventQueue
class EventQueue {
public:
    Tick getCurTick() const { return _curTick; }
    void schedule(Event *e, Tick when);
    void deschedule(Event *e);
    void reschedule(Event *e, Tick when) { if (e->scheduled()) deschedule(e); schedule(e, when); }
    bool empty() const { return _live == 0; }
    Tick nextTick() const;
    // 执行队首事件；返回 false 表示队列已空
    bool serviceOne();
    uint64_t numEvents = 0;
private:
    struct Cmp {
        bool operator()(const Event *a, const Event *b) const {
            if (a->_when != b->_when) return a->_when > b->_when;
            if (a->_prio != b->_prio) return a->_prio > b->_prio;
            return a->_seq > b->_seq;
        }
    };
    std::priority_queue<Event *, std::vector<Event *>, Cmp> _q;
    Tick _curTick = 0;
    uint64_t _seq = 0;
    size_t _live = 0;
};

EventQueue &mainEventQueue();
inline Tick curTick() { return mainEventQueue().getCurTick(); }

// --------------------------------------------------------------- Stats
namespace stats {
struct Info {
    std::string name, desc;
    virtual ~Info() = default;
    virtual std::string str() const = 0;
    virtual double value() const = 0;
};
struct Scalar : Info {
    double v = 0;
    Scalar &operator+=(double d) { v += d; return *this; }
    Scalar &operator++() { v += 1; return *this; }
    Scalar &operator=(double d) { v = d; return *this; }
    std::string str() const override;
    double value() const override { return v; }
};
struct Vector : Info {
    std::vector<double> v;
    std::vector<std::string> labels;
    void init(size_t n) { v.assign(n, 0); labels.assign(n, ""); }
    double &operator[](size_t i) { return v[i]; }
    double total() const;
    std::string str() const override;
    double value() const override { return total(); }
};
struct Registry {
    std::vector<Info *> all;
    Scalar &scalar(const std::string &name, const std::string &desc = "");
    Vector &vector(const std::string &name, size_t n, const std::string &desc = "");
    void dump(std::ostream &os) const;
    void dumpJson(std::ostream &os) const;
    Info *find(const std::string &name) const;
};
Registry &registry();
}  // namespace stats

// ------------------------------------------------------------ SimObject
class SimObject {
public:
    explicit SimObject(std::string name);
    virtual ~SimObject() = default;
    const std::string &name() const { return _name; }
    virtual void init() {}
    virtual void startup() {}
    virtual void regStats() {}
    static std::vector<SimObject *> &all();
protected:
    stats::Scalar &statScalar(const std::string &n, const std::string &d = "") {
        return stats::registry().scalar(_name + "." + n, d);
    }
    stats::Vector &statVector(const std::string &n, size_t k, const std::string &d = "") {
        return stats::registry().vector(_name + "." + n, k, d);
    }
private:
    std::string _name;
};

class ClockedObject : public SimObject {
public:
    ClockedObject(std::string name, Tick period) : SimObject(std::move(name)), _period(period) {}
    Tick clockPeriod() const { return _period; }
    Cycles curCycle() const { return curTick() / _period; }
    // 当前或下一个时钟沿之后 n 个周期的 tick
    Tick clockEdge(Cycles n = 0) const {
        Tick t = curTick();
        Tick edge = (t + _period - 1) / _period * _period;
        return edge + n * _period;
    }
    Tick nextCycle() const { return clockEdge(1) == curTick() ? curTick() + _period : clockEdge(curTick() % _period == 0 ? 1 : 0); }
    Tick cyclesToTicks(Cycles c) const { return c * _period; }
    Cycles ticksToCycles(Tick t) const { return (t + _period - 1) / _period; }
private:
    Tick _period;
};

// -------------------------------------------------------------- Params
// 极简 JSON 解析（对象 / 数组 / 数字 / 字符串 / 布尔 / null），用于读取 configs/*.json
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    static Json parse(const std::string &text);
    static Json parseFile(const std::string &path);
    const Json *get(const std::string &key) const {
        if (type != Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    bool has(const std::string &k) const { return get(k) != nullptr; }
};

// 参数容器：先从 JSON 段加载，再被 --set 覆盖；类型化读取带默认值
class Params {
public:
    void loadJson(const Json &section);
    void set(const std::string &key, const std::string &value);
    bool has(const std::string &k) const { return _kv.count(k) != 0; }
    int64_t getInt(const std::string &k, int64_t def) const;
    double getDouble(const std::string &k, double def) const;
    bool getBool(const std::string &k, bool def) const;
    std::string getString(const std::string &k, const std::string &def) const;
    const std::map<std::string, std::string> &raw() const { return _kv; }
private:
    std::map<std::string, std::string> _kv;
};

[[noreturn]] void fatal(const std::string &msg);
void warn(const std::string &msg);
extern bool verboseWarn;

}  // namespace sim
