#include "sim.hh"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace sim {

bool verboseWarn = true;
void fatal(const std::string &msg) { std::cerr << "fatal: " << msg << std::endl; std::exit(2); }
void warn(const std::string &msg) { if (verboseWarn) std::cerr << "warn: " << msg << std::endl; }

// ------------------------------------------------------------ EventQueue
EventQueue &mainEventQueue() { static EventQueue q; return q; }

void EventQueue::schedule(Event *e, Tick when) {
    if (e->_scheduled) throw std::logic_error("event already scheduled: " + e->name());
    if (when < _curTick) throw std::logic_error("event scheduled in the past: " + e->name());
    e->_when = when;
    e->_seq = _seq++;
    e->_scheduled = true;
    e->_squashed = false;
    _q.push(e);
    ++_live;
}

void EventQueue::deschedule(Event *e) {
    if (!e->_scheduled) return;
    e->_scheduled = false;
    e->_squashed = true;      // 惰性删除：出队时跳过
    --_live;
}

Tick EventQueue::nextTick() const { return _q.empty() ? MaxTick : _q.top()->_when; }

bool EventQueue::serviceOne() {
    while (!_q.empty()) {
        Event *e = _q.top();
        _q.pop();
        if (e->_squashed) { e->_squashed = false; continue; }
        _curTick = e->_when;
        e->_scheduled = false;
        --_live;
        ++numEvents;
        e->process();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- Stats
namespace stats {
Registry &registry() { static Registry r; return r; }

std::string Scalar::str() const { std::ostringstream os; os << v; return os.str(); }
double Vector::total() const { double s = 0; for (double x : v) s += x; return s; }
std::string Vector::str() const {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) os << ", ";
        os << (labels[i].empty() ? std::to_string(i) : labels[i]) << "=" << v[i];
    }
    return os.str();
}
Scalar &Registry::scalar(const std::string &name, const std::string &desc) {
    auto *s = new Scalar(); s->name = name; s->desc = desc; all.push_back(s); return *s;
}
Vector &Registry::vector(const std::string &name, size_t n, const std::string &desc) {
    auto *s = new Vector(); s->name = name; s->desc = desc; s->init(n); all.push_back(s); return *s;
}
Info *Registry::find(const std::string &name) const {
    for (auto *i : all) if (i->name == name) return i;
    return nullptr;
}
void Registry::dump(std::ostream &os) const {
    for (auto *i : all) {
        os << std::left << std::setw(44) << i->name << " " << std::setw(24) << i->str();
        if (!i->desc.empty()) os << " # " << i->desc;
        os << "\n";
    }
}
void Registry::dumpJson(std::ostream &os) const {
    os << "{";
    bool first = true;
    for (auto *i : all) {
        if (!first) os << ",";
        first = false;
        os << "\n  \"" << i->name << "\": ";
        if (auto *v = dynamic_cast<Vector *>(i)) {
            os << "[";
            for (size_t k = 0; k < v->v.size(); ++k) os << (k ? "," : "") << v->v[k];
            os << "]";
        } else {
            os << i->value();
        }
    }
    os << "\n}\n";
}
}  // namespace stats

// ------------------------------------------------------------ SimObject
std::vector<SimObject *> &SimObject::all() { static std::vector<SimObject *> v; return v; }
SimObject::SimObject(std::string name) : _name(std::move(name)) { all().push_back(this); }

// ----------------------------------------------------------------- JSON
namespace {
struct JParser {
    const std::string &s; size_t i = 0;
    explicit JParser(const std::string &t) : s(t) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    [[noreturn]] void err(const std::string &m) { throw std::runtime_error("json: " + m + " at " + std::to_string(i)); }
    Json value() {
        ws();
        if (i >= s.size()) err("eof");
        char c = s[i];
        Json j;
        if (c == '{') {
            j.type = Json::Object; ++i; ws();
            if (s[i] == '}') { ++i; return j; }
            while (true) {
                ws(); if (s[i] != '"') err("key");
                std::string k = string();
                ws(); if (s[i] != ':') err("colon"); ++i;
                j.obj[k] = value();
                ws();
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; return j; }
                err("object");
            }
        }
        if (c == '[') {
            j.type = Json::Array; ++i; ws();
            if (s[i] == ']') { ++i; return j; }
            while (true) {
                j.arr.push_back(value()); ws();
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; return j; }
                err("array");
            }
        }
        if (c == '"') { j.type = Json::String; j.str = string(); return j; }
        if (s.compare(i, 4, "true") == 0) { j.type = Json::Bool; j.b = true; i += 4; return j; }
        if (s.compare(i, 5, "false") == 0) { j.type = Json::Bool; j.b = false; i += 5; return j; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return j; }
        size_t st = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
        if (st == i) err("value");
        j.type = Json::Number; j.num = std::strtod(s.substr(st, i - st).c_str(), nullptr);
        return j;
    }
    std::string string() {
        ++i; std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) { ++i; char e = s[i]; out += (e == 'n' ? '\n' : e == 't' ? '\t' : e); }
            else out += s[i];
            ++i;
        }
        ++i;
        return out;
    }
};
}  // namespace

Json Json::parse(const std::string &text) { JParser p(text); return p.value(); }
Json Json::parseFile(const std::string &path) {
    std::ifstream f(path);
    if (!f) fatal("cannot open " + path);
    std::stringstream ss; ss << f.rdbuf();
    return parse(ss.str());
}

// --------------------------------------------------------------- Params
void Params::loadJson(const Json &sec) {
    if (sec.type != Json::Object) return;
    for (auto &[k, v] : sec.obj) {
        if (k.rfind("_", 0) == 0) continue;
        switch (v.type) {
            case Json::Bool: _kv[k] = v.b ? "true" : "false"; break;
            case Json::Number: { std::ostringstream os; os << std::setprecision(17) << v.num; _kv[k] = os.str(); break; }
            case Json::String: _kv[k] = v.str; break;
            default: break;
        }
    }
}
void Params::set(const std::string &key, const std::string &value) { _kv[key] = value; }
int64_t Params::getInt(const std::string &k, int64_t def) const {
    auto it = _kv.find(k); if (it == _kv.end()) return def;
    return (int64_t)std::llround(std::strtod(it->second.c_str(), nullptr));
}
double Params::getDouble(const std::string &k, double def) const {
    auto it = _kv.find(k); if (it == _kv.end()) return def;
    return std::strtod(it->second.c_str(), nullptr);
}
bool Params::getBool(const std::string &k, bool def) const {
    auto it = _kv.find(k); if (it == _kv.end()) return def;
    const std::string &v = it->second;
    return v == "true" || v == "1" || v == "on" || v == "yes";
}
std::string Params::getString(const std::string &k, const std::string &def) const {
    auto it = _kv.find(k); return it == _kv.end() ? def : it->second;
}

}  // namespace sim
