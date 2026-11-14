#include "gb_songs.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "../i18n.h"
#include "../vgm.h"
#include "gb.h"

namespace plumnin {

namespace {

constexpr double kClock = 4194304.0;
// Where the driver returns to when it is called by itself: memory nothing runs from.
constexpr uint16_t kTrap = 0xFEA0;

// The memory a driver keeps its state in: work RAM and high RAM, as 0..kRam-1.
constexpr int kRam = 0x2080;
using RamSet = std::bitset<kRam>;
int ramIndex(uint16_t a) {
    if (a >= 0xC000 && a < 0xE000) return a - 0xC000;
    if (a >= 0xFF80 && a < 0xFFFF) return 0x2000 + (a - 0xFF80);
    return -1;
}
uint16_t ramAddress(int i) { return static_cast<uint16_t>(i < 0x2000 ? 0xC000 + i : 0xFF80 + (i - 0x2000)); }

// A routine of the game: its address with the ROM bank it was in (0xFFFF for RAM).
uint32_t routineId(const GameBoy& gb, uint16_t a) {
    const uint32_t bank = a < 0x4000 ? 0 : a < 0x8000 ? static_cast<uint32_t>(gb.romBank()) : 0xFFFF;
    return bank << 16 | a;
}

std::string hexId(uint32_t id) {
    char s[16];
    std::snprintf(s, sizeof s, "%02X:%04X", (id >> 16) & 0xFF, id & 0xFFFF);
    return s;
}

struct Routine {
    uint64_t calls = 0, instructions = 0, sound = 0;
    uint64_t firstClock = 0, lastClock = 0;
    uint64_t quickCalls = 0;  // within 1/200 s of the one before
    bool underDriver = false, aboveDriver = false;
    RamSet wrote;  // outside the driver
    std::array<std::set<uint8_t>, 3> arguments;  // A, C and E as it was called
};

// Watches the game run: which routines write the chip, and, once the driver is known,
// what memory it reads and writes and what the rest of the game writes.
class Watcher : public GameBoy::Tracer {
public:
    explicit Watcher(GameBoy& gb) : gb_(gb) {}

    uint32_t driver = 0;
    bool trackMemory = false;
    std::unordered_map<uint32_t, Routine> routines;
    uint64_t soundWrites = 0;
    uint64_t lastNote = 0;  // when the last note started, on the sound clock
    int mostNotesPerSecond = 0;
    RamSet driverWrote, driverRead;
    std::array<uint32_t, kRam> outsideWrites{};
    std::array<uint32_t, kRam> consumed{};
    std::array<std::set<uint8_t>, kRam> requests;  // the values the game asked with
    std::array<std::set<uint8_t>, kRam> written;   // the values the rest of the game wrote
    // Called on each call, with the routine and how many times it has been called.
    std::function<void(uint32_t, uint64_t)> onCall;

    void instruction(uint16_t, uint16_t sp) override {
        sp_ = sp;
        while (!stack_.empty() && sp > stack_.back().sp) pop();
        for (const Frame& f : stack_) ++f.routine->instructions;
    }
    void call(uint16_t to, uint16_t sp, bool interrupt) override {
        sp_ = sp;
        while (!stack_.empty() && sp >= stack_.back().sp) pop();  // a routine jumped out of
        const uint32_t id = routineId(gb_, to);
        Routine& r = routines[id];
        if (!r.calls) r.firstClock = gb_.soundClock();
        else if (gb_.soundClock() - r.lastClock < static_cast<uint64_t>(kClock / 200)) ++r.quickCalls;
        r.lastClock = gb_.soundClock();
        ++r.calls;
        if (trackMemory) {
            const GameBoy::Cpu c = gb_.cpu();
            int i = 0;
            for (uint8_t v : {c.a, c.c, c.e})
                if (r.arguments[static_cast<size_t>(i++)].size() < 12) r.arguments[static_cast<size_t>(i - 1)].insert(v);
        }
        if (driver) {
            if (inDriver()) r.underDriver = true;
            // What calls it, down to the interrupt it runs in: what an interrupt
            // happened to stop is not calling it.
            if (id == driver && !interrupt)
                for (auto f = stack_.rbegin(); f != stack_.rend(); ++f) {
                    if (f->interrupt) break;
                    f->routine->aboveDriver = true;
                }
        }
        stack_.push_back({id, sp, &r, interrupt});
        if (driver && id == driver) ++driverCalls_;
        if (onCall) onCall(id, r.calls);
    }
    void access(uint16_t a, uint8_t v, bool write) override {
        if (write && a >= 0xFF10 && a < 0xFF40) {
            ++soundWrites;
            if ((a == 0xFF14 || a == 0xFF19 || a == 0xFF1E || a == 0xFF23) && (v & 0x80)) {
                const uint64_t second = gb_.soundClock() / static_cast<uint64_t>(kClock);
                if (second != noteSecond_) noteSecond_ = second, notesThisSecond_ = 0;
                mostNotesPerSecond = std::max(mostNotesPerSecond, ++notesThisSecond_);
                lastNote = gb_.soundClock();
            }
            uint32_t last = 0;
            for (const Frame& f : stack_)
                if (f.id != last) ++f.routine->sound, last = f.id;
            return;
        }
        if (!trackMemory) return;
        const int i = ramIndex(a);
        if (i < 0) return;
        // What a call or a push puts on the stack is not the driver's state.
        if (a + 3 >= sp_ && a <= sp_ + 1) return;
        const size_t k = static_cast<size_t>(i);
        if (inDriver()) {
            (write ? driverWrote : driverRead).set(k);
            // A request taken: what the game wrote, cleared (or marked) by the driver.
            // (At once, on the driver's next call: a counter run down later is no request.)
            if (write && pending_[k] && driverCalls_ - pendingCall_[k] <= 1 && (v == 0 || v == 0xFF || v == (pending_[k] ^ 0x80))) {
                ++consumed[k];
                if (requests[k].size() < 8) requests[k].insert(pending_[k]);
                pending_[k] = 0;
            }
        } else if (write) {
            ++outsideWrites[k];
            pending_[k] = v;
            pendingCall_[k] = driverCalls_;
            if (written[k].size() < 8) written[k].insert(v);
            for (const Frame& f : stack_) f.routine->wrote.set(static_cast<size_t>(i));
        }
    }

private:
    struct Frame {
        uint32_t id;
        uint16_t sp;
        Routine* routine;
        bool interrupt;
    };
    void pop() {
        stack_.pop_back();
    }
    GameBoy& gb_;
    std::array<uint8_t, kRam> pending_{};
    std::array<uint64_t, kRam> pendingCall_{};
    uint64_t driverCalls_ = 0;
    uint64_t noteSecond_ = 0;
    int notesThisSecond_ = 0;
    std::vector<Frame> stack_;
    // The driver is running: it is on the stack, and no interrupt came in over it (some
    // games run the rest of the game in an interrupt that the driver's lets in).
    bool inDriver() const {
        if (!driver) return false;
        for (auto f = stack_.rbegin(); f != stack_.rend(); ++f) {
            if (f->id == driver) return true;
            if (f->interrupt) return false;
        }
        return false;
    }
    uint16_t sp_ = 0xFFFE;
};

// Listens to what is written to the chip: the notes started, on which channels.
class Listener : public GameBoy::Tracer {
public:
    int triggers = 0;
    uint8_t channels = 0;
    uint32_t signature = 2166136261u;
    uint32_t lateSignature = 2166136261u;  // of the notes after `late` was set
    uint32_t opening = 2166136261u;        // of the first 24, wherever they fall
    bool late = false;
    uint64_t writes = 0;
    uint64_t writeHash = 1469598103934665603ull;  // of every write since it was last reset
    std::vector<uint32_t> started;  // the notes started (channel and pitch) since it was last cleared
    std::map<uint32_t, int> lateNotes;  // how many of each after `late` was set

    void instruction(uint16_t, uint16_t) override {}
    void call(uint16_t, uint16_t, bool) override {}
    void access(uint16_t a, uint8_t v, bool write) override {
        if (!write || a < 0xFF10 || a >= 0xFF40) return;
        ++writes;
        const int r = a - 0xFF10;
        writeHash = (writeHash ^ (static_cast<uint64_t>(r) << 8 | v)) * 1099511628211ull;
        regs_[r] = v;
        if ((r == 0x04 || r == 0x09 || r == 0x0E || r == 0x13) && (v & 0x80)) {
            const int ch = r / 5;
            ++triggers;
            channels = static_cast<uint8_t>(channels | 1 << ch);
            const uint32_t pitch = ch == 3 ? regs_[0x12] : static_cast<uint32_t>(regs_[r - 1] | (v & 7) << 8);
            if (started.size() < 64) started.push_back(static_cast<uint32_t>(ch) << 16 | pitch);
            if (late) ++lateNotes[static_cast<uint32_t>(ch) << 16 | pitch];
            for (uint32_t x : {static_cast<uint32_t>(ch), pitch & 0xFF, pitch >> 8}) {
                signature = (signature ^ x) * 16777619u;
                if (late) lateSignature = (lateSignature ^ x) * 16777619u;
                if (triggers <= 24) opening = (opening ^ x) * 16777619u;
            }
        }
    }
    int channelCount() const {
        int n = 0;
        for (int i = 0; i < 4; ++i) n += (channels >> i) & 1;
        return n;
    }

private:
    uint8_t regs_[0x30] = {};
};

// The driver run by itself: its routine called as often as the game calls it, with the
// banks it runs with, and nothing else of the game running.
struct Driver {
    uint32_t routine = 0;
    GameBoy::Banks banks;
    double rate = 60;  // calls a second
    RamSet state;      // the memory it reads back, which decides what it plays next

    // One call. False if it did not come back.
    bool call(GameBoy& gb) const {
        GameBoy::Cpu c = gb.cpu();
        const uint16_t sp = static_cast<uint16_t>(c.sp - 2);
        gb.poke(sp, kTrap & 0xFF);
        gb.poke(static_cast<uint16_t>(sp + 1), kTrap >> 8);
        c.sp = sp;
        c.pc = static_cast<uint16_t>(routine);
        c.ime = false;
        gb.setBanks(banks);
        gb.setCpu(c);
        // Nothing of the game's own runs meanwhile, even if the driver enables interrupts.
        gb.setInterruptEnable(0);
        gb.clearInterrupts();
        for (int n = 0; n < 300000; ++n) {
            const GameBoy::Cpu now = gb.cpu();
            if (now.pc == kTrap && now.sp == static_cast<uint16_t>(sp + 2)) {
                gb.setInterruptEnable(0);
                return true;
            }
            gb.clearInterrupts();
            gb.stepInstruction();
        }
        return false;
    }
    // Calls it, then lets the chip run until the next call is due.
    bool tick(GameBoy& gb, uint64_t& next, bool chip) const {
        if (!call(gb)) return false;
        next += static_cast<uint64_t>(kClock / rate);
        if (gb.soundClock() < next) gb.idleSound(next - gb.soundClock(), chip);
        return true;
    }
    uint64_t stateHash(const GameBoy& gb) const {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < kRam; ++i)
            if (state[static_cast<size_t>(i)]) h = (h ^ gb.peek(ramAddress(i))) * 1099511628211ull;
        return h;
    }
};

// One way of asking for a song: calling a routine with the number, or writing a byte.
struct Way {
    bool routine = true;
    uint32_t id = 0;       // the routine, or the byte's index in RAM
    std::vector<uint8_t> state;  // the console just as the routine is called, or the driver is
    std::set<uint8_t> asked;     // the numbers the game itself asked for this way
    bool whilePlaying = false;   // a byte, with the driver playing what the game asked last
    int reg = 0;                 // a routine: which it is asked in, A, C or E
    std::string describe() const {
        char s[64];
        if (routine) std::snprintf(s, sizeof s, "rutina %s (%c)", hexId(id).c_str(), "ACE"[reg]);
        else std::snprintf(s, sizeof s, "byte %04X%s", ramAddress(static_cast<int>(id)), whilePlaying ? "*" : "");
        return s;
    }
};

struct Heard {
    bool ok = false;
    int triggers = 0, channels = 0;
    int late = 0;  // notes started after the first second and a half: not a sound effect
    // Music: notes on two channels or more, still coming after a sound effect would end.
    // Not many more notes than the game's own music has either, which is the driver
    // reading what is not a song.
    bool song(int most) const { return ok && channels >= 2 && triggers >= 8 && late > 0 && triggers <= most; }
    uint32_t signature = 0, lateSignature = 0, opening = 0;
    std::map<uint32_t, int> lateNotes;
    // Much the same notes late on as `other`, at much the same times or not: the same
    // song going on, with an effect over its start.
    bool goesOnAs(const Heard& other) const {
        int differ = 0, total = 0;
        for (const auto& [key, n] : lateNotes) {
            auto it = other.lateNotes.find(key);
            differ += std::abs(n - (it == other.lateNotes.end() ? 0 : it->second));
            total += n;
        }
        for (const auto& [key, n] : other.lateNotes)
            if (!lateNotes.count(key)) differ += n;
        return total > 0 && 4 * differ <= total;
    }
};

class Ripper {
public:
    Ripper(const std::vector<uint8_t>& rom, const GbSongRipper::Options& o, GbSongRipper::Progress* p)
        : rom_(rom), options_(o), progress_(p) {}

    bool run(std::vector<GbSongRipper::Song>* songs, std::string* error, std::string* report);

private:
    bool boot(GameBoy& gb, std::string* error) {
        if (!gb.loadRomData(rom_, error)) return false;
        gb.setSampleRate(44100);
        return true;
    }
    // Runs the game from power on for the time it is watched.
    void watch(GameBoy& gb, Watcher& w) {
        gb.setTracer(&w);
        const int frames = static_cast<int>(options_.traceSeconds * gb.refreshRate());
        for (int i = 0; i < frames && !cancelled(); ++i) {
            // Past the title screen, where many games are silent: Start, then A, now
            // and then for the first seconds, the same on every run.
            PadState pad;
            const int second = i / 60, frame = i % 60;
            pad.start = second < 20 && second % 4 == 3 && frame < 6;
            pad.a = second < 20 && second % 4 == 1 && frame < 6;
            gb.setPads(pad, PadState{});
            gb.runFrame();
            gb.audio().clearSamples();
        }
        gb.setTracer(nullptr);
    }
    bool cancelled() const { return progress_ && progress_->cancel.load(); }
    void step() {
        if (progress_) ++progress_->step;
        if (std::getenv("PLUMNIN_RIP_DEBUG") && progress_ && progress_->step % 32 == 0)
            std::fprintf(stderr, "step %d at %.1f s\n", progress_ ? progress_->step.load() : 0,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
    }
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

    // From `way`'s state, asks for song `number`: returns false if that could not be done.
    bool ask(GameBoy& gb, const Way& way, int number) {
        std::string error;
        if (!gb.loadState(way.state, &error)) return false;
        if (!way.routine) {
            if (number >= 0) gb.poke(ramAddress(static_cast<int>(way.id)), static_cast<uint8_t>(number));
            gb.setInterruptEnable(0);
            return true;
        }
        GameBoy::Cpu c = gb.cpu();
        const uint16_t back = static_cast<uint16_t>(gb.peek(c.sp) | gb.peek(static_cast<uint16_t>(c.sp + 1)) << 8);
        const uint16_t sp = static_cast<uint16_t>(c.sp + 2);
        if (number < 0) {  // not called at all: as if it came back at once
            c.pc = back;
            c.sp = sp;
            gb.setCpu(c);
            gb.setInterruptEnable(0);
            return true;
        }
        // The rest as the game had them: some give the bank of the song in another.
        (way.reg == 0 ? c.a : way.reg == 1 ? c.c : c.e) = static_cast<uint8_t>(number);
        gb.setCpu(c);
        // The routine runs as in the game, interrupts and all, until it returns.
        for (int n = 0; n < 2000000; ++n) {
            const GameBoy::Cpu now = gb.cpu();
            if (now.pc == back && now.sp == sp) {
                gb.setInterruptEnable(0);
                return true;
            }
            gb.stepInstruction();
        }
        return false;
    }

    Heard listen(GameBoy& gb, const Way& way, int number) {
        Heard h;
        if (!ask(gb, way, number)) return h;
        Listener l;
        gb.setTracer(&l);
        uint64_t next = gb.soundClock();
        const int ticks = static_cast<int>(options_.probeSeconds * driver_.rate);
        bool ok = true;
        int early = 0;
        for (int t = 0; t < ticks && ok; ++t) {
            if (t == static_cast<int>(1.5 * driver_.rate)) early = l.triggers, l.late = true;
            ok = driver_.tick(gb, next, false);
            // Nothing in the first second: nothing at all, most likely.
            if (t == static_cast<int>(driver_.rate) && l.triggers == 0) break;
        }
        gb.setTracer(nullptr);
        h.ok = ok;
        h.triggers = l.triggers;
        h.late = early ? l.triggers - early : 0;
        h.channels = l.channelCount();
        h.signature = l.signature;
        h.lateSignature = l.lateSignature;
        h.opening = l.opening;
        h.lateNotes = std::move(l.lateNotes);
        return h;
    }

    // `label` numbers it among the songs found, for its title.
    // A song recorded, to be written out once it is known which are kept.
    struct Take {
        std::unique_ptr<VgmWriter> vgm;
        VgmWriter::Mark end, loop;
        bool looped = false;
        bool unfinished = false;  // neither looped nor ended, as long as it was recorded
        bool inStep = true;       // unfinished, but with channels going round together
        int number = 0;
        const Way* way = nullptr;
        std::vector<uint32_t> played;  // the notes it plays, in order, and those of its loop once more
    };
    bool record(GameBoy& gb, const Way& way, int number, Take* take);

    std::vector<uint8_t> rom_;
    GbSongRipper::Options options_;
    GbSongRipper::Progress* progress_;
    Driver driver_;
    std::string title_;
    int mostNotes_ = 120;  // in a probe, from how many the game's music has
};

// Whether the end of `h` repeats, the last `period` entries the same as the ones before
// them (and before those, as far back as it goes): `from` is where that starts.
bool repeats(const std::vector<uint64_t>& h, int shortest, int* from, int* period) {
    const int n = static_cast<int>(h.size());
    for (int p = shortest; 2 * p <= n; ++p) {
        if (h[static_cast<size_t>(n - 1)] != h[static_cast<size_t>(n - 1 - p)]) continue;
        bool same = true;
        for (int k = n - p; k < n && same; ++k) same = h[static_cast<size_t>(k)] == h[static_cast<size_t>(k - p)];
        if (!same) continue;
        // Something must happen in it: a stretch of silence repeats too.
        bool varied = false;
        for (int k = n - p + 1; k < n && !varied; ++k) varied = h[static_cast<size_t>(k)] != h[static_cast<size_t>(n - p)];
        if (!varied) continue;
        int i = n - 2 * p;
        while (i > 0 && h[static_cast<size_t>(i - 1)] == h[static_cast<size_t>(i - 1 + p)]) --i;
        *from = i;
        *period = p;
        return true;
    }
    return false;
}

// A note: when it started, in ticks, and which (channel and pitch).
struct Note {
    int tick;
    uint32_t key;
};

// Whether the notes repeat, give or take a tick: drivers that keep fractions of a tick
// for their tempo never come back to quite the same state, nor write quite the same in
// each tick, but play the same notes. The last `period` ticks the same as the ones
// before, for at least `least` ticks back: `from` is where that starts.
bool repeatsLoosely(const std::vector<Note>& notes, int ticks, int shortest, int least, int* from, int* period) {
    const auto at = [&](int tick) {
        return std::lower_bound(notes.begin(), notes.end(), tick, [](const Note& a, int t) { return a.tick < t; });
    };
    // Is a note like `n` heard `d` ticks from it, give or take one?
    const auto heard = [&](const Note& n, int d) {
        for (auto it = at(n.tick + d - 1); it != notes.end() && it->tick <= n.tick + d + 1; ++it)
            if (it->key == n.key) return true;
        return false;
    };
    for (int p = shortest; 2 * p <= ticks; ++p) {
        const int span = std::max(p, least);
        if (span + p > ticks) break;
        // Every note of the last `span` ticks heard `p` before, and every one of those
        // heard `p` after: the same notes, not just some of them (all but one in fifty,
        // for a drum a tick or two off).
        const auto last = at(ticks - span), before = at(ticks - span - p);
        const long count = notes.end() - last;
        if (count < 16) continue;
        long wrong = 0;
        const long allowed = count / 50;
        for (auto it = last; it != notes.end() && wrong <= allowed; ++it) wrong += !heard(*it, -p);
        for (auto it = before; it != notes.end() && it->tick < ticks - p - 1 && wrong <= allowed; ++it) wrong += !heard(*it, p);
        if (wrong > allowed) continue;
        // Where it starts: as far back as the notes are heard again a period later.
        auto first = at(ticks - span - p);
        int start = ticks - span - p;
        while (first != notes.begin() && (first - 1)->tick + p < ticks && heard(*(first - 1), p)) start = (--first)->tick;
        *from = start;
        *period = p;
        return true;
    }
    return false;
}

bool Ripper::record(GameBoy& gb, const Way& way, int number, Take* take) {
    std::string error;
    if (!gb.loadState(way.state, &error)) return false;
    take->vgm = std::make_unique<VgmWriter>();
    VgmWriter& vgm = *take->vgm;
    gb.recordMusic(&vgm);
    // The state is loaded again by ask(): the recording starts with the chip as it is.
    if (!ask(gb, way, number)) {
        gb.recordMusic(nullptr);
        return false;
    }
    uint64_t next = gb.soundClock();
    const int maxTicks = static_cast<int>(options_.maxSeconds * driver_.rate);
    const int minLoop = static_cast<int>(2 * driver_.rate);
    std::vector<VgmWriter::Mark> marks;
    std::unordered_map<uint64_t, int> seen;
    Listener l;
    gb.setTracer(&l);
    int lastSound = 0, loopFrom = -1, end = -1;
    uint64_t triggers = 0;
    std::vector<int> notes;  // notes started before each tick
    std::vector<uint64_t> perTick;  // what was written in each
    std::vector<Note> started;
    for (int t = 0; t < maxTicks; ++t) {
        marks.push_back(vgm.mark(gb.soundClock()));
        notes.push_back(l.triggers);
        const uint64_t h = driver_.stateHash(gb);
        if (auto it = seen.find(h); it != seen.end() && t - it->second >= minLoop) {
            loopFrom = it->second;
            end = t;
            break;
        }
        seen.emplace(h, t);
        l.writeHash = 1469598103934665603ull;
        if (!driver_.tick(gb, next, true)) break;
        perTick.push_back(l.writeHash);
        for (uint32_t key : l.started) started.push_back({t, key});
        l.started.clear();
        // A driver that counts something forever never comes back to the same state: then
        // the loop is where what it writes repeats, every ten seconds from the thirtieth.
        if (t >= static_cast<int>(30 * driver_.rate) && t % static_cast<int>(10 * driver_.rate) == 0) {
            int from = -1, period = 0;
            if (repeats(perTick, minLoop, &from, &period) ||
                repeatsLoosely(started, t + 1, minLoop, static_cast<int>(12 * driver_.rate), &from, &period)) {
                loopFrom = from;
                end = from + period;
                break;
            }
        }
        if (static_cast<uint64_t>(l.triggers) != triggers) triggers = static_cast<uint64_t>(l.triggers), lastSound = t;
        // Silent for long, with every channel off (or for longer, with them on and
        // nothing to play): it has ended.
        if ((t - lastSound > static_cast<int>(4 * driver_.rate) && (gb.peek(0xFF26) & 0x0F) == 0) ||
            t - lastSound > static_cast<int>(8 * driver_.rate)) {
            end = std::min(t, lastSound + static_cast<int>(1.5 * driver_.rate));
            break;
        }
        if (cancelled()) break;
    }
    gb.setTracer(nullptr);
    gb.recordMusic(nullptr);
    // Back where it was with no note in between: it ended, and stays silent.
    if (loopFrom >= 0 && notes[static_cast<size_t>(loopFrom)] == notes[static_cast<size_t>(end)]) {
        end = std::min(end, lastSound + static_cast<int>(1.5 * driver_.rate));
        loopFrom = -1;
    }
    take->unfinished = end < 0 && !cancelled();
    if (take->unfinished) {
        // Some of its channels going round in the same time, even if not all: a song,
        // with a part that never quite comes back the same. None: the driver playing what
        // is not a song, each channel on its own.
        std::vector<int> periods;
        for (uint32_t channel = 0; channel < 4; ++channel) {
            std::vector<Note> own;
            for (const Note& n : started)
                if (n.key >> 16 == channel) own.push_back(n);
            int from = 0, period = 0;
            if (own.size() >= 20 &&
                repeatsLoosely(own, static_cast<int>(marks.size()), minLoop, static_cast<int>(12 * driver_.rate), &from, &period))
                periods.push_back(period);
        }
        take->inStep = false;
        for (size_t i = 0; i < periods.size(); ++i)
            for (size_t j = i + 1; j < periods.size(); ++j) take->inStep = take->inStep || std::abs(periods[i] - periods[j]) <= 2;
    }
    if (end < 0) end = static_cast<int>(marks.size()) - 1;
    if (end <= 0) return false;
    for (const Note& n : started)
        if (n.tick < end) take->played.push_back(n.key);
    if (loopFrom >= 0)
        for (const Note& n : started)
            if (n.tick >= loopFrom && n.tick < end) take->played.push_back(n.key);
    take->number = number;
    take->end = marks[static_cast<size_t>(end)];
    take->looped = loopFrom >= 0;
    if (take->looped) take->loop = marks[static_cast<size_t>(loopFrom)];
    return true;
}

bool Ripper::run(std::vector<GbSongRipper::Song>* songs, std::string* error, std::string* report) {
    std::string log;
    // 1. Who writes the chip: the driver is the routine under which nearly all of it is
    // written, called often, that runs the least.
    // (On the heap, as everything big here: this runs on a thread of its own.)
    auto gbOwn = std::make_unique<GameBoy>();
    GameBoy& gb = *gbOwn;
    if (!boot(gb, error)) return false;
    title_ = gb.title();
    if (progress_) progress_->steps = 3 + 256;
    auto firstOwn = std::make_unique<Watcher>(gb);
    Watcher& first = *firstOwn;
    watch(gb, first);
    step();
    if (cancelled()) return false;
    if (first.soundWrites < 50) {
        if (error) *error = tr("El juego no ha tocado música en el tiempo que se ha mirado.");
        return false;
    }
    // (Some games split it: the music in one bank, the effects in another, called from
    // a routine of the fixed bank; that one, which writes the most, is the driver.)
    uint32_t driver = 0;
    uint64_t most = 0;
    double best = 0;
    for (const auto& [id, r] : first.routines) {
        if (r.calls < 30 || r.sound * 10 < first.soundWrites * 4) continue;
        const double seconds = static_cast<double>(r.lastClock - r.firstClock) / kClock;
        const double rate = seconds > 0 ? static_cast<double>(r.calls - 1) / seconds : 0;
        if (rate < 20) continue;
        // Called at a steady pace, not twice in a row (once for each channel, say).
        if (std::getenv("PLUMNIN_RIP_DEBUG"))
            std::fprintf(stderr, "candidate %s: rate %.1f, %llu quick\n", hexId(id).c_str(), rate,
                         static_cast<unsigned long long>(r.quickCalls));
        if (r.quickCalls * 10 > r.calls) continue;  // (a few, when the game lags)
        const double cost = static_cast<double>(r.instructions) / static_cast<double>(r.calls);
        // As much as the most, give or take a twentieth, and the least work for it.
        const bool more = r.sound * 20 > most * 21, asMuch = r.sound * 21 >= most * 20;
        if (!driver || more || (asMuch && cost < best)) {
            driver = id, best = cost, driver_.rate = rate;
            most = std::max(most, r.sound);
        }
    }
    if (std::getenv("PLUMNIN_RIP_DEBUG")) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (const auto& [id, r] : first.routines) top.push_back({r.sound, id});
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "%llu writes to the chip\n", static_cast<unsigned long long>(first.soundWrites));
        for (size_t i = 0; i < top.size() && i < 12; ++i) {
            const Routine& r = first.routines[top[i].second];
            std::fprintf(stderr, "  %s: %llu writes, %llu calls over %.1f s, %.0f instructions\n", hexId(top[i].second).c_str(),
                         static_cast<unsigned long long>(r.sound), static_cast<unsigned long long>(r.calls),
                         static_cast<double>(r.lastClock - r.firstClock) / kClock,
                         static_cast<double>(r.instructions) / static_cast<double>(std::max<uint64_t>(1, r.calls)));
        }
    }
    if (!driver) {
        if (error) *error = tr("No se ha encontrado el controlador de sonido del juego.");
        return false;
    }
    driver_.routine = driver;
    mostNotes_ = std::max(120, static_cast<int>(1.5 * first.mostNotesPerSecond * options_.probeSeconds));
    log += "controlador: " + hexId(driver) + ", " + std::to_string(static_cast<int>(driver_.rate + 0.5)) + " veces por segundo\n";

    // 2. Again, now knowing the driver: what memory is its own, and who else writes it.
    auto gb2Own = std::make_unique<GameBoy>();
    GameBoy& gb2 = *gb2Own;
    boot(gb2, nullptr);
    auto secondOwn = std::make_unique<Watcher>(gb2);
    Watcher& second = *secondOwn;
    second.driver = driver;
    second.trackMemory = true;
    watch(gb2, second);
    step();
    if (cancelled()) return false;
    driver_.state = second.driverWrote & second.driverRead;
    const Routine& d = second.routines[driver];
    std::vector<std::pair<double, uint32_t>> starters;
    for (const auto& [id, r] : second.routines) {
        if (id == driver || r.underDriver || r.aboveDriver || r.calls > 64 || (id & 0xFFFF) < 0x68) continue;
        if ((r.wrote & second.driverWrote).count() < 2) continue;
        starters.push_back({static_cast<double>(r.instructions) / static_cast<double>(r.calls), id});
    }
    if (std::getenv("PLUMNIN_RIP_DEBUG")) {
        std::fprintf(stderr, "driver state: %zu bytes written, %zu read\n", second.driverWrote.count(), second.driverRead.count());
        for (const auto& [id, r] : second.routines) {
            const size_t n = (r.wrote & second.driverWrote).count();
            if (n >= 2)
                std::fprintf(stderr, "  writer %s: %zu bytes, %llu calls, under %d above %d\n", hexId(id).c_str(), n,
                             static_cast<unsigned long long>(r.calls), r.underDriver, r.aboveDriver);
        }
        for (int i = 0; i < kRam; ++i) {
            const size_t k = static_cast<size_t>(i);
            if (second.driverRead[k] && second.outsideWrites[k] && std::getenv("PLUMNIN_RIP_DEBUG")[0] == '2') {
                std::string vals;
                for (uint8_t v : second.written[k]) vals += " " + std::to_string(v);
                std::fprintf(stderr, "  input %04X: written %u:%s, driver writes %d\n", ramAddress(i), second.outsideWrites[k], vals.c_str(), (int)second.driverWrote[k]);
            }
        }
        for (int i = 0; i < kRam; ++i)
            if (second.consumed[static_cast<size_t>(i)])
                std::fprintf(stderr, "  consumed %04X: %u, written %u, read %d\n", ramAddress(i), second.consumed[static_cast<size_t>(i)],
                             second.outsideWrites[static_cast<size_t>(i)], static_cast<int>(second.driverRead[static_cast<size_t>(i)]));
    }
    std::sort(starters.begin(), starters.end());
    if (starters.size() > 24) starters.resize(24);
    // The bytes the game writes and the driver reads and then clears: requests.
    std::vector<std::pair<uint32_t, int>> bytes;
    for (int i = 0; i < kRam; ++i) {
        const size_t k = static_cast<size_t>(i);
        const uint32_t n = second.outsideWrites[k];
        if (second.driverRead[k] && second.consumed[k] && n <= 400) bytes.push_back({n, i});
    }
    // Else any the game writes and the driver reads: the numbers the game itself asks
    // with tell the real ones from the rest further on.
    const bool asked = !bytes.empty();
    if (!asked)
        for (int i = 0; i < kRam; ++i) {
            const size_t k = static_cast<size_t>(i);
            const uint32_t n = second.outsideWrites[k];
            // Only ever cleared is no request; those written with more numbers first.
            size_t numbers = 0;
            for (uint8_t v : second.written[k]) numbers += v != 0 && v != 0xFF;
            if (second.driverRead[k] && numbers && n <= 400) bytes.push_back({static_cast<uint32_t>(8 - std::min<size_t>(numbers, 8)) << 16 | n, i});
        }
    std::sort(bytes.begin(), bytes.end());
    if (bytes.size() > 16) bytes.resize(16);

    // 3. Once more, keeping the console as it is when each of those is called.
    std::vector<Way> ways;
    for (const auto& s : starters) {
        const auto& arguments = second.routines[s.second].arguments;
        for (int reg = 0; reg < 3; ++reg) {
            // (The same numbers in another: most likely the same, copied.)
            bool copy = false;
            for (int other = 0; other < reg; ++other) copy = copy || arguments[static_cast<size_t>(other)] == arguments[static_cast<size_t>(reg)];
            if (copy) continue;
            Way w{true, s.second, {}, arguments[static_cast<size_t>(reg)]};
            w.reg = reg;
            ways.push_back(w);
        }
    }
    const size_t routines = ways.size();
    for (const auto& b : bytes)
        ways.push_back({false, static_cast<uint32_t>(b.second), {},
                        asked ? second.requests[static_cast<size_t>(b.second)] : second.written[static_cast<size_t>(b.second)]});
    {
        auto gb3Own = std::make_unique<GameBoy>();
        GameBoy& gb3 = *gb3Own;
        boot(gb3, nullptr);
        auto thirdOwn = std::make_unique<Watcher>(gb3);
        Watcher& third = *thirdOwn;
        third.driver = driver;
        const uint64_t driverCall = d.calls > 10 ? d.calls - 5 : d.calls;
        // The driver as it is when it plays nothing, so that what a request starts is
        // heard on its own; and as late as it was watched, in case it was silent then
        // because it was told to be.
        std::vector<uint8_t> silentState, lateState;
        third.onCall = [&](uint32_t id, uint64_t calls) {
            if (calls == 1)
                for (size_t i = 0; i < routines; ++i)
                    if (ways[i].id == id) gb3.saveState(ways[i].state, true);
            if (id != driver) return;
            // (Silent after it has played something: before, it may not be ready.)
            const bool quiet = calls >= 30 && third.lastNote > 0 &&
                               gb3.soundClock() - third.lastNote > static_cast<uint64_t>(kClock);
            if (quiet && silentState.empty()) gb3.saveState(silentState, true);
            // (Now and then as well: saving a state can shift the run by a few calls.)
            if (calls == driverCall || (calls <= driverCall && calls % 64 == 0)) {
                gb3.saveState(lateState, true);
                driver_.banks = gb3.banks();
            }
        };
        watch(gb3, third);
        if (lateState.empty()) {
            if (error) *error = tr("No se ha encontrado el controlador de sonido del juego.");
            return false;
        }
        std::vector<Way> late;
        for (size_t i = routines; i < ways.size(); ++i) {
            late.push_back(ways[i]);
            late.back().state = lateState;
            late.back().whilePlaying = true;
            ways[i].state = silentState;
        }
        if (silentState.empty()) ways.resize(routines);
        ways.insert(ways.end(), late.begin(), late.end());
    }
    step();

    // 4. Every number, every way; the songs are what starts notes on two channels or
    // more, each heard once.
    struct Found {
        const Way* way;
        int number;
    };
    std::vector<Found> found;
    std::set<uint32_t> signatures, openings;
    auto probeOwn = std::make_unique<GameBoy>();
    GameBoy& probe = *probeOwn;
    boot(probe, nullptr);
    std::map<const Way*, Heard> baseline;
    const auto sameAsBefore = [&](const Way& w, const Heard& h) {
        const Heard& b = baseline[&w];
        return h.lateSignature == b.lateSignature || h.goesOnAs(b);
    };
    // What the driver plays with the byte left as it was, or the routine not called: not a
    // new song, nor is what ends up the same after a moment (an effect over it).
    for (const Way& w : ways) baseline[&w] = listen(probe, w, -1);
    // A way's songs are numbered from a table: past its end, what the driver plays is
    // whatever follows it. So a way is given up after a few numbers in a row that play
    // next to nothing, once it has given songs.
    std::map<const Way*, std::pair<int, int>> tally;  // songs, dead numbers in a row
    // The ways that play a song with a number the game itself asked for are the real
    // ones.
    std::vector<const Way*> real;
    for (const Way& w : ways) {
        bool ok = false;
        for (uint8_t n : w.asked) {
            if (n == 0) continue;  // asking for nothing, or a byte merely cleared
            const Heard h = listen(probe, w, n);
            ok = ok || (h.song(mostNotes_) && !sameAsBefore(w, h));
        }
        if (std::getenv("PLUMNIN_RIP_DEBUG")) {
            std::string asked;
            for (uint8_t n : w.asked) asked += " " + std::to_string(n);
            std::fprintf(stderr, "%s asked%s: %s\n", w.describe().c_str(), asked.c_str(), ok ? "real" : "-");
        }
        // A byte asked for with the driver silent is the same byte while it plays.
        if (ok && w.whilePlaying)
            for (const Way* r : real)
                if (!r->routine && r->id == w.id) ok = false;
        if (ok) real.push_back(&w);
    }
    // None: what the others play would be anything but the game's songs.
    if (real.empty()) {
        if (error) *error = tr("Se ha encontrado el controlador de sonido, pero no cómo pide el juego las canciones.");
        return false;
    }
    for (int number = 0; number < 256 && !cancelled(); ++number) {
        for (const Way* way : real) {
            const Way& w = *way;
            auto& [songsHere, dead] = tally[&w];
            if (songsHere >= 2 && dead >= 4) continue;
            const Heard h = listen(probe, w, number);
            dead = h.song(mostNotes_) ? 0 : dead + 1;
            if (std::getenv("PLUMNIN_RIP_DEBUG") && number < 40 && !w.routine)
                std::fprintf(stderr, "  %d via %s: ok %d, %d notes on %d channels\n", number, w.describe().c_str(), h.ok,
                             h.triggers, h.channels);
            if (!h.song(mostNotes_)) continue;
            if (sameAsBefore(w, h)) continue;
            // The same song again, or started a moment sooner or later.
            if (!signatures.insert(h.signature).second || !openings.insert(h.opening).second) continue;
            found.push_back({&w, number});
            ++songsHere;
            if (std::getenv("PLUMNIN_RIP_DEBUG"))
                std::fprintf(stderr, "found %d via %s: %d notes on %d channels\n", number, w.describe().c_str(), h.triggers,
                             h.channels);
            if (progress_) ++progress_->found;
            break;
        }
        step();
    }
    if (found.empty()) {
        if (error) *error = tr("Se ha encontrado el controlador de sonido, pero no cómo pide el juego las canciones.");
        return false;
    }

    // 5. Each one recorded until it loops.
    if (progress_) progress_->steps = progress_->step + static_cast<int>(found.size());
    // A song heard before, taken up somewhere along it (a way that starts no song of its
    // own, just lets the one playing go on): some notes from its middle, played in that
    // order in one recorded already.
    std::vector<std::vector<uint32_t>> heard;
    const auto again = [&](const std::vector<uint32_t>& notes) {
        const size_t length = 48, from = notes.size() / 3;
        if (notes.size() < from + length) return false;
        // (On more than one channel: a beat of drums alone is in many.)
        std::set<uint32_t> channels;
        for (size_t i = from; i < from + length; ++i) channels.insert(notes[i] >> 16);
        if (channels.size() < 2) return false;
        for (size_t k = 0; k < heard.size(); ++k)
            if (std::search(heard[k].begin(), heard[k].end(), notes.begin() + static_cast<long>(from),
                            notes.begin() + static_cast<long>(from + length)) != heard[k].end()) {
                if (std::getenv("PLUMNIN_RIP_DEBUG")) std::fprintf(stderr, "  as song %zu\n", k + 1);
                return true;
            }
        return false;
    };
    std::map<const Way*, int> byWay;
    std::vector<Take> takes;
    for (const Found& f : found) {
        if (cancelled()) return false;
        Take take;
        if (record(probe, *f.way, f.number, &take)) {
            const bool before = again(take.played);
            if (std::getenv("PLUMNIN_RIP_DEBUG"))
                std::fprintf(stderr, "song %d: %s%.1f s, loop %d%s\n", f.number, before ? "heard before, " : "",
                             static_cast<double>(take.end.samples) / 44100.0, take.looped, take.unfinished ? (take.inStep ? ", unfinished" : ", unfinished, out of step") : "");
            if (!before) {
                heard.push_back(take.played);
                take.way = f.way;
                take.played.clear();
                takes.push_back(std::move(take));
            }
        }
        step();
    }
    // Songs never coming back to where they were, when the others do: the driver playing
    // what is not a song (each channel going round on its own, out of step).
    bool loops = false;
    for (const Take& t : takes) loops = loops || t.looped;
    for (Take& t : takes) {
        if (loops && t.unfinished && !t.inStep) continue;
        ++byWay[t.way];
        GbSongRipper::Song song;
        VgmWriter::Tag tag{trf("Canción %02d", static_cast<int>(songs->size()) + 1), title_, trf("Número %d del juego", t.number)};
        song.number = t.number;
        song.vgz = t.vgm->finish(tag, &t.end, t.looped ? &t.loop : nullptr);
        song.seconds = static_cast<double>(t.end.samples) / 44100.0;
        song.loopSeconds = t.looped ? static_cast<double>(t.end.samples - t.loop.samples) / 44100.0 : 0;
        songs->push_back(std::move(song));
        t.vgm.reset();
    }
    for (const auto& [w, n] : byWay) log += w->describe() + ": " + std::to_string(n) + " canciones\n";
    if (report) *report = log;
    return !songs->empty();
}

}  // namespace

bool GbSongRipper::run(const std::vector<uint8_t>& rom, const Options& options, Progress* progress,
                       std::vector<Song>* songs, std::string* error, std::string* report) {
    songs->clear();
    Ripper r(rom, options, progress);
    const bool ok = r.run(songs, error, report);
    if (!ok && error && error->empty() && progress && progress->cancel) *error = tr("Cancelado.");
    return ok;
}

}  // namespace plumnin
