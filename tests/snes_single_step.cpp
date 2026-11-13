// Runs SingleStepTests' tests of the 65C816 (https://github.com/SingleStepTests/65816)
// and of the SPC700 (https://github.com/SingleStepTests/spc700) on plumnin's SNES
// processors: one instruction from a known state, and the state, the memory and the
// bus cycles it must end with. They are not part of plumnin: give the folder with their
// JSON files (a file cut short is fine: the tests before the cut are run).
//
//   snes-single-step 65816 FOLDER [--limit N] [--cycles] [--verbose]
//   snes-single-step spc700 FOLDER [--limit N] [--cycles] [--verbose]
//
// The exit code is 1 if any test fails. --cycles also compares each bus cycle (its kind,
// address and value), not only how many there are.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "snes/snes_cpu.h"
#include "snes/snes_smp.h"

namespace {

// A JSON value, just what these files hold: objects, arrays, numbers, strings and null.
struct Json {
    enum class Type { Null, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json* get(const char* key) const {
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    long long num(const char* key) const {
        const Json* j = get(key);
        return j ? static_cast<long long>(j->number) : 0;
    }
};

class Parser {
public:
    Parser(const char* p, const char* end) : p_(p), end_(end) {}
    bool value(Json& out) {
        skip();
        if (p_ >= end_) return false;
        const char c = *p_;
        if (c == '{') {
            ++p_;
            out.type = Json::Type::Object;
            skip();
            if (p_ < end_ && *p_ == '}') return ++p_, true;
            for (;;) {
                skip();
                Json key;
                if (!string(key)) return false;
                skip();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                Json v;
                if (!value(v)) return false;
                out.fields.emplace_back(key.text, std::move(v));
                skip();
                if (p_ >= end_) return false;
                if (*p_ == ',') {
                    ++p_;
                    continue;
                }
                if (*p_ == '}') return ++p_, true;
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            out.type = Json::Type::Array;
            skip();
            if (p_ < end_ && *p_ == ']') return ++p_, true;
            for (;;) {
                Json v;
                if (!value(v)) return false;
                out.items.push_back(std::move(v));
                skip();
                if (p_ >= end_) return false;
                if (*p_ == ',') {
                    ++p_;
                    continue;
                }
                if (*p_ == ']') return ++p_, true;
                return false;
            }
        }
        if (c == '"') return string(out);
        if (c == 'n') {
            if (end_ - p_ < 4) return false;
            p_ += 4;
            out.type = Json::Type::Null;
            return true;
        }
        char* stop = nullptr;
        const std::string digits(p_, static_cast<size_t>(std::min<ptrdiff_t>(end_ - p_, 32)));
        out.number = std::strtod(digits.c_str(), &stop);
        if (stop == digits.c_str()) return false;
        p_ += stop - digits.c_str();
        out.type = Json::Type::Number;
        return p_ < end_;
    }
    // The tests of a file: the elements of its top array, as far as the file goes.
    template <class F>
    void eachTest(F&& f) {
        skip();
        if (p_ >= end_ || *p_ != '[') return;
        ++p_;
        for (;;) {
            Json t;
            if (!value(t)) return;
            f(t);
            skip();
            if (p_ >= end_ || *p_ != ',') return;
            ++p_;
        }
    }

private:
    void skip() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    bool string(Json& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        out.type = Json::Type::String;
        while (p_ < end_ && *p_ != '"') out.text += *p_++;
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    const char* p_;
    const char* end_;
};

enum class Kind { Read, Write, Idle };
struct Cycle {
    Kind kind;
    uint32_t address;
    int value;
};

struct Results {
    int passed = 0, failed = 0;
};

// --- 65C816 ---------------------------------------------------------------------------

class FlatBus : public plumnin::snes::CpuBus {
public:
    std::unordered_map<uint32_t, uint8_t> ram;
    std::vector<Cycle> cycles;
    uint8_t read(uint32_t a) override {
        auto it = ram.find(a);
        const uint8_t v = it == ram.end() ? 0 : it->second;
        cycles.push_back({Kind::Read, a, v});
        return v;
    }
    void write(uint32_t a, uint8_t v) override {
        ram[a] = v;
        cycles.push_back({Kind::Write, a, v});
    }
    void idle() override { cycles.push_back({Kind::Idle, 0, -1}); }
};

std::vector<Cycle> expectedCycles(const Json& t, bool spc) {
    std::vector<Cycle> out;
    const Json* c = t.get("cycles");
    if (!c) return out;
    for (const Json& e : c->items) {
        if (e.items.size() < 3) continue;
        const uint32_t a = static_cast<uint32_t>(e.items[0].number);
        const int v = e.items[1].type == Json::Type::Null ? -1 : static_cast<int>(e.items[1].number);
        const std::string& f = e.items[2].text;
        Kind k;
        if (spc) k = f == "write" ? Kind::Write : f == "read" ? Kind::Read : Kind::Idle;
        else if (f.size() >= 4 && f[3] == 'w') k = Kind::Write;
        else k = f.size() >= 2 && (f[0] == 'd' || f[1] == 'p' || f[2] == 'v') ? Kind::Read : Kind::Idle;
        out.push_back({k, a, v});
    }
    return out;
}

bool sameCycles(const std::vector<Cycle>& got, const std::vector<Cycle>& want, bool detail) {
    if (got.size() != want.size()) return false;
    if (!detail) return true;
    for (size_t i = 0; i < got.size(); ++i) {
        // An idle cycle is a read whose value nobody looks at.
        if (got[i].kind == Kind::Idle && want[i].kind != Kind::Write && want[i].value < 0) continue;
        if (got[i].kind != want[i].kind) return false;
        if (got[i].kind == Kind::Idle) continue;
        if (got[i].address != want[i].address) return false;
        if (want[i].value >= 0 && got[i].value != want[i].value) return false;
    }
    return true;
}

bool run65816(const Json& t, bool detail, std::string* why) {
    using plumnin::snes::Cpu;
    const Json* in = t.get("initial");
    const Json* out = t.get("final");
    if (!in || !out) return true;
    FlatBus bus;
    Cpu cpu;
    cpu.setBus(&bus);
    Cpu::Regs& r = cpu.r;
    r.pc = static_cast<uint16_t>(in->num("pc"));
    r.s = static_cast<uint16_t>(in->num("s"));
    r.p = static_cast<uint8_t>(in->num("p"));
    r.a = static_cast<uint16_t>(in->num("a"));
    r.x = static_cast<uint16_t>(in->num("x"));
    r.y = static_cast<uint16_t>(in->num("y"));
    r.db = static_cast<uint8_t>(in->num("dbr"));
    r.d = static_cast<uint16_t>(in->num("d"));
    r.pb = static_cast<uint8_t>(in->num("pbr"));
    r.e = in->num("e") != 0;
    if (r.e) r.s = static_cast<uint16_t>(0x100 | (r.s & 0xFF));
    if (const Json* ram = in->get("ram"))
        for (const Json& e : ram->items) bus.ram[static_cast<uint32_t>(e.items[0].number)] = static_cast<uint8_t>(e.items[1].number);
    const std::vector<Cycle> want = expectedCycles(t, false);
    const uint8_t op = bus.ram[static_cast<uint32_t>(r.pb) << 16 | r.pc];
    const uint16_t startPc = r.pc;
    cpu.step();
    // A block move copies one byte an instruction: it runs until it is done.
    if (op == 0x44 || op == 0x54)
        while (r.pc == startPc && bus.cycles.size() < want.size()) cpu.step();
    // WAI and STP: the tests also count the first cycle spent waiting.
    if (op == 0xCB || op == 0xDB) cpu.step();
    std::ostringstream o;
    auto check = [&](const char* name, long long got, long long expected) {
        if (got != expected) o << " " << name << "=" << std::hex << got << " (want " << expected << ")" << std::dec;
    };
    check("pc", r.pc, out->num("pc"));
    check("s", r.s, out->num("s"));
    check("p", r.p, out->num("p"));
    check("a", r.a, out->num("a"));
    check("x", r.x, out->num("x"));
    check("y", r.y, out->num("y"));
    check("dbr", r.db, out->num("dbr"));
    check("d", r.d, out->num("d"));
    check("pbr", r.pb, out->num("pbr"));
    check("e", r.e, out->num("e"));
    if (const Json* ram = out->get("ram"))
        for (const Json& e : ram->items) {
            const uint32_t a = static_cast<uint32_t>(e.items[0].number);
            char name[32];
            std::snprintf(name, sizeof name, "[%06x]", a);
            check(name, bus.ram[a], static_cast<long long>(e.items[1].number));
        }
    if (!sameCycles(bus.cycles, want, detail)) o << " cycles " << bus.cycles.size() << " (want " << want.size() << ")";
    *why = o.str();
    return why->empty();
}

// --- SPC700 ---------------------------------------------------------------------------

class SpcFlatBus : public plumnin::snes::SmpBus {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000, 0);
    std::vector<Cycle> cycles;
    uint8_t read(uint16_t a) override {
        cycles.push_back({Kind::Read, a, ram[a]});
        return ram[a];
    }
    void write(uint16_t a, uint8_t v) override {
        ram[a] = v;
        cycles.push_back({Kind::Write, a, v});
    }
    void idle() override { cycles.push_back({Kind::Idle, 0, -1}); }
};

bool runSpc700(const Json& t, bool detail, std::string* why) {
    using plumnin::snes::Smp;
    const Json* in = t.get("initial");
    const Json* out = t.get("final");
    if (!in || !out) return true;
    SpcFlatBus bus;
    Smp smp;
    smp.setBus(&bus);
    Smp::Regs& r = smp.r;
    r.pc = static_cast<uint16_t>(in->num("pc"));
    r.a = static_cast<uint8_t>(in->num("a"));
    r.x = static_cast<uint8_t>(in->num("x"));
    r.y = static_cast<uint8_t>(in->num("y"));
    r.sp = static_cast<uint8_t>(in->num("sp"));
    r.p = static_cast<uint8_t>(in->num("psw"));
    if (const Json* ram = in->get("ram"))
        for (const Json& e : ram->items) bus.ram[static_cast<uint16_t>(e.items[0].number)] = static_cast<uint8_t>(e.items[1].number);
    const uint8_t op = bus.ram[r.pc];
    const std::vector<Cycle> want = expectedCycles(t, true);
    smp.step();
    // SLEEP and STOP: the tests go on counting the cycles spent stopped.
    if (op == 0xEF || op == 0xFF)
        while (bus.cycles.size() < want.size()) smp.step();
    std::ostringstream o;
    auto check = [&](const char* name, long long got, long long expected) {
        if (got != expected) o << " " << name << "=" << std::hex << got << " (want " << expected << ")" << std::dec;
    };
    check("pc", r.pc, out->num("pc"));
    check("a", r.a, out->num("a"));
    check("x", r.x, out->num("x"));
    check("y", r.y, out->num("y"));
    check("sp", r.sp, out->num("sp"));
    check("psw", r.p, out->num("psw"));
    if (const Json* ram = out->get("ram"))
        for (const Json& e : ram->items) {
            const uint16_t a = static_cast<uint16_t>(e.items[0].number);
            char name[32];
            std::snprintf(name, sizeof name, "[%04x]", a);
            check(name, bus.ram[a], static_cast<long long>(e.items[1].number));
        }
    if (!sameCycles(bus.cycles, want, detail)) o << " cycles " << bus.cycles.size() << " (want " << want.size() << ")";
    *why = o.str();
    return why->empty();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: snes-single-step 65816|spc700 FOLDER [--limit N] [--cycles] [--verbose]\n");
        return 2;
    }
    const std::string which = argv[1];
    const bool spc = which == "spc700";
    if (!spc && which != "65816") {
        std::fprintf(stderr, "unknown processor %s\n", which.c_str());
        return 2;
    }
    const std::filesystem::path folder = argv[2];
    int limit = 1 << 30;
    bool detail = false, verbose = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) limit = std::atoi(argv[++i]);
        else if (a == "--cycles") detail = true;
        else if (a == "--verbose") verbose = true;
    }
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec))
        if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .json files in %s\n", folder.string().c_str());
        return 2;
    }
    Results total;
    int badFiles = 0;
    for (const auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Parser parser(text.data(), text.data() + text.size());
        Results res;
        std::string firstFailure;
        int count = 0;
        parser.eachTest([&](const Json& t) {
            if (count++ >= limit) return;
            std::string why;
            const bool ok = spc ? runSpc700(t, detail, &why) : run65816(t, detail, &why);
            if (ok) {
                ++res.passed;
            } else {
                ++res.failed;
                if (firstFailure.empty()) {
                    const Json* name = t.get("name");
                    firstFailure = (name ? name->text : "?") + ":" + why;
                }
            }
        });
        total.passed += res.passed;
        total.failed += res.failed;
        if (res.failed) ++badFiles;
        if (res.failed || verbose)
            std::printf("%-14s %5d passed %5d failed  %s\n", path.filename().string().c_str(), res.passed, res.failed,
                        firstFailure.c_str());
    }
    std::printf("%s: %d of %d tests passed, %d of %zu files with failures\n", which.c_str(), total.passed,
                total.passed + total.failed, badFiles, files.size());
    return total.failed ? 1 : 0;
}
