// Sacar canciones on the Super Nintendo. Its music is played by a processor of its own,
// the SPC700, with a driver the game loads into it; the game asks it for a song through
// four ports, after loading the song's notes and samples if they are not there yet. So
// the songs are asked for as the game itself asks: the game is run for a while, watching
// what it writes to the ports and which byte of its memory each value came from; then,
// from just before the game wrote that byte, it is run again with another number written
// in its place, for every number. What starts other music on several voices is a song,
// and the sound processor as the song starts, saved as an SPC, plays it by itself.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "i18n.h"
#include "snes/snes.h"
#include "songs.h"
#include "synth_music.h"

namespace plumnin {

namespace {

constexpr int kWindow = 360;     // frames a number is listened to, from where the game asked
constexpr size_t kRing = 120;    // frames back a byte may have been written before it is sent
constexpr int kMaxTests = 120;   // bytes tried, at most
constexpr size_t kLoadReads = 12;  // bytes read before a song is loaded taken for its number
constexpr uint32_t kSilence = 30;  // frames without a note before music that begins anew
constexpr size_t kMaxOnsets = 32;  // moments music began, kept from the trace
constexpr int kMaxStarts = 6;    // moments a byte that asks for songs is tried from
constexpr int kDeadNumbers = 24;  // numbers in a row with no new song before giving up
constexpr int kEffectWindow = 90;     // frames a sound effect is listened to
constexpr size_t kMaxEffectWays = 4;  // bytes taken for the ways of asking for them, at most
constexpr int kEffectNumbers = 96;    // numbers tried on each
constexpr int kDeadEffects = 12;      // in a row with nothing added, once some did
constexpr int kFewSongs = 2;          // an instrument of effects that only so many songs play is theirs

// A key on, as the game is run frame by frame.
struct KeyOn {
    uint32_t frame;
    uint8_t voice, sample;
    uint16_t pitch;
    uint32_t instrument;  // the synthesizer's fingerprint of what it plays
    bool operator<(const KeyOn& o) const {
        return std::tie(frame, voice, sample, pitch) < std::tie(o.frame, o.voice, o.sample, o.pitch);
    }
};

// The voices keyed on since `seen`, added to `out`.
void keyOns(snes::Apu& apu, std::array<uint32_t, 8>& seen, std::vector<KeyOn>* out, uint32_t frame) {
    snes::Dsp& d = apu.dsp();
    for (int i = 0; i < 8; ++i) {
        const uint32_t k = d.keyOns(i);
        if (k == seen[static_cast<size_t>(i)]) continue;
        seen[static_cast<size_t>(i)] = k;
        if (!out) continue;
        SynthMusic::VoiceState state;
        d.musicVoice(i, state);
        out->push_back({frame, static_cast<uint8_t>(i), d.read(static_cast<uint8_t>(i * 16 + 4)),
                        static_cast<uint16_t>(d.read(static_cast<uint8_t>(i * 16 + 2)) |
                                              (d.read(static_cast<uint8_t>(i * 16 + 3)) & 0x3F) << 8),
                        state.fingerprint});
    }
}

// The instruments of the notes in `with` that are not in `without` (the same moment, the
// same notes but for what a request added).
std::set<uint32_t> added(std::vector<KeyOn> with, std::vector<KeyOn> without, uint32_t from) {
    std::sort(with.begin(), with.end());
    std::sort(without.begin(), without.end());
    std::vector<KeyOn> extra;
    std::set_difference(with.begin(), with.end(), without.begin(), without.end(), std::back_inserter(extra));
    std::set<uint32_t> instruments;
    for (const KeyOn& k : extra)
        if (k.frame >= from && k.instrument) instruments.insert(k.instrument);
    return instruments;
}

// What played in a stretch of frames.
struct Heard {
    int notes = 0, voices = 0, late = 0;
    std::array<std::vector<uint32_t>, 8> lines;  // each voice's first notes (sample and pitch)
    uint64_t signature = 0;
    // Music: notes on two voices or more, still going at the end.
    bool music() const { return notes >= 10 && voices >= 2 && late >= 3; }
    // On how many voices it is not what `o` was: a sound effect over the music changes one
    // or two, another song most.
    int differs(const Heard& o) const {
        int n = 0;
        for (size_t v = 0; v < 8; ++v) n += lines[v] != o.lines[v];
        return n;
    }
};

Heard judge(const std::vector<KeyOn>& keys, uint32_t from, uint32_t to) {
    Heard h;
    uint32_t voices = 0;
    for (const KeyOn& k : keys) {
        if (k.frame < from || k.frame >= to) continue;
        ++h.notes;
        voices |= 1u << k.voice;
        if (k.frame + 60 >= to) ++h.late;
        auto& line = h.lines[k.voice];
        if (line.size() < 8) line.push_back(k.sample * 0x10000u + (k.pitch >> 7));
    }
    uint64_t sig = 1469598103934665603ull;
    for (const auto& line : h.lines) {
        for (uint32_t x : line) sig = (sig ^ x) * 1099511628211ull;
        sig = (sig ^ 0xFFFF) * 1099511628211ull;
    }
    for (uint32_t v = voices; v; v &= v - 1) ++h.voices;
    h.signature = sig;
    return h;
}

// A byte of the work RAM the game sent to a port: where, the value, the frame it was
// written (and the console just before that frame) and the frame it was sent.
struct Asked {
    uint32_t index = 0;
    uint8_t value = 0;
    uint32_t written = 0, sent = 0;
    std::shared_ptr<const std::vector<uint8_t>> state;
    // Set into the memory as the state is loaded, not as the game writes it: a byte the
    // game read just before it loaded a song into the sound processor (games that keep one
    // song there at a time ask for it so, not with a number at the ports).
    bool poke = false;
};

class SnesRipper {
public:
    SnesRipper(const std::vector<uint8_t>& rom, const SongRipper::Options& o, SongRipper::Progress* p)
        : rom_(rom), options_(o), progress_(p) {}

    bool run(std::vector<SongRipper::Song>* songs, std::string* error, std::string* report, std::set<uint32_t>* effects);

private:
    bool cancelled() const { return progress_ && progress_->cancel.load(); }
    void step() {
        if (progress_) ++progress_->step;
    }
    void trace();
    // The game run again from `a`'s moment, the byte written as `number` (none if < 0), for
    // `extra` frames past where the game sent it; its key ons, and with `spcs` the sound
    // processor at the end of every frame.
    void listen(const Asked& a, int number, std::vector<KeyOn>* keys, std::vector<std::vector<uint8_t>>* spcs,
                int window = kWindow);
    // From the sound processor as the song starts: how long to play it (its intro and loop
    // twice, or to its end), and what it is wherever it was started. False if too short.
    // `instruments` gets those of its notes.
    bool measure(const std::vector<uint8_t>& spc, double* seconds, uint64_t* identity, std::string* log,
                 std::set<uint32_t>* instruments);

    const std::vector<uint8_t>& rom_;
    SongRipper::Options options_;
    SongRipper::Progress* progress_;
    std::unique_ptr<Snes> snes_;
    std::vector<Asked> asked_;
    // The sound processor just before music began in the trace (after a silence, or once a
    // song was loaded): the songs the game played itself, for when no way of asking for
    // them is found.
    std::vector<std::vector<uint8_t>> onsets_;
};

void SnesRipper::trace() {
    Snes& s = *snes_;
    struct Read {
        uint32_t index;
        uint8_t value;
        bool stack;  // off the stack (a return address, a register saved)
    };
    std::deque<Read> reads;
    std::vector<uint32_t> lastWrite(0x20000, UINT32_MAX);
    std::deque<std::shared_ptr<const std::vector<uint8_t>>> ring;  // the console as each of the last frames began
    std::array<uint8_t, 4> last{};
    std::vector<Asked> pending;
    std::map<uint32_t, int> perIndex;
    int portWrites = 0;
    // A song loaded into the sound processor, a frame of lots sent: the bytes the game read
    // just before it began, one of them perhaps the song's number.
    std::vector<Read> beforeLoad;
    std::vector<Asked> loads;
    bool loading = false;
    int bursts = 0;
    s.wramReadWatch = [&](uint32_t i, uint8_t v) {
        const uint32_t sp = s.cpuRegs().s;
        reads.push_back({i, v, i > sp && i <= sp + 0x40});
        if (reads.size() > 48) reads.pop_front();
    };
    s.wramWatch = [&](uint32_t i, uint8_t&) { lastWrite[i] = static_cast<uint32_t>(s.frames()); };
    s.apu().portWatch = [&](int p, uint8_t v) {
        if (portWrites++ == 0) beforeLoad.assign(reads.begin(), reads.end());
        if (last[static_cast<size_t>(p)] == v) return;
        last[static_cast<size_t>(p)] = v;
        if (v == 0) return;
        // The bytes just read that held the value sent.
        std::set<uint32_t> seen;
        const uint32_t now = static_cast<uint32_t>(s.frames());
        for (auto it = reads.rbegin(); it != reads.rend(); ++it) {
            if (it->value != v || !seen.insert(it->index).second) continue;
            const uint32_t written = lastWrite[it->index];
            if (written == UINT32_MAX || now - written >= ring.size()) continue;
            pending.push_back({it->index, v, written, now, ring[ring.size() - 1 - (now - written)]});
        }
    };
    std::array<uint32_t, 8> keyed{};
    for (int i = 0; i < 8; ++i) keyed[static_cast<size_t>(i)] = s.apu().dsp().keyOns(i);
    uint32_t lastNote = 0;
    bool loaded = false;  // a song loaded, its first note not heard yet
    std::vector<uint8_t> previous = s.musicSnapshot({}, 0);
    const int frames = static_cast<int>(std::max(options_.traceSeconds, 80.0) * 60);
    for (int f = 0; f < frames && !cancelled(); ++f) {
        auto state = std::make_shared<std::vector<uint8_t>>();
        s.saveState(*state, true);
        ring.push_back(std::move(state));
        if (ring.size() > kRing) ring.pop_front();
        // The title and the demo first; then Start now and then, into the game.
        PadState pad;
        pad.start = f > 1800 && f % 240 < 4;
        s.setPads(pad, PadState{});
        portWrites = 0;
        const size_t before = pending.size();
        s.runFrame();
        s.audio().clearSamples();
        // (Not what the game sends while it loads the driver or the music: lots a frame.)
        if (portWrites > 64) pending.resize(before);
        for (size_t i = before; i < pending.size(); ++i)
            if (perIndex[pending[i].index]++ < 4) asked_.push_back(pending[i]);
        pending.clear();
        // A load begun: the bytes read before it, the latest first, each once. Once it is
        // over, they are asked for from the frame it began.
        const uint32_t now = static_cast<uint32_t>(s.frames());
        // (Not the first: the driver itself, loaded as the game starts.)
        if (portWrites > 64 && !loading && !ring.empty() && ++bursts > 1) {
            std::set<uint32_t> seen;
            for (auto it = beforeLoad.rbegin(); it != beforeLoad.rend() && seen.size() < kLoadReads; ++it)
                if (!it->stack && seen.insert(it->index).second)
                    loads.push_back({it->index, it->value, now - 1, now - 1, ring.back(), true});
        }
        // Music begun: after a while with no note, or the first note of a song loaded.
        bool note = false;
        for (int i = 0; i < 8; ++i) {
            const uint32_t k = s.apu().dsp().keyOns(i);
            note = note || k != keyed[static_cast<size_t>(i)];
            keyed[static_cast<size_t>(i)] = k;
        }
        if (note) {
            if ((loaded || now - lastNote > kSilence) && onsets_.size() < kMaxOnsets) onsets_.push_back(previous);
            loaded = false;
            lastNote = now;
        }
        previous = s.musicSnapshot({}, 0);
        if (loading && portWrites <= 64) loaded = true;
        loading = portWrites > 64;
        if (!loading && !loads.empty()) {
            for (Asked& a : loads) {
                a.sent = now;
                if (perIndex[a.index + 0x100000]++ < 2) asked_.push_back(a);
            }
            loads.clear();
        }
    }
    s.wramReadWatch = nullptr;
    s.wramWatch = nullptr;
    s.apu().portWatch = nullptr;
    if (asked_.size() > static_cast<size_t>(kMaxTests)) asked_.resize(kMaxTests);
}

void SnesRipper::listen(const Asked& a, int number, std::vector<KeyOn>* keys, std::vector<std::vector<uint8_t>>* spcs,
                        int window) {
    Snes& s = *snes_;
    s.loadState(*a.state, nullptr);
    if (a.poke && number >= 0) s.setWramByte(a.index, static_cast<uint8_t>(number));
    bool done = number < 0 || a.poke;
    s.wramWatch = [&](uint32_t i, uint8_t& v) {
        if (!done && i == a.index && v == a.value) {
            v = static_cast<uint8_t>(number);
            done = true;
        }
    };
    std::array<uint32_t, 8> seen{};
    for (int i = 0; i < 8; ++i) seen[static_cast<size_t>(i)] = s.apu().dsp().keyOns(i);
    const uint32_t frames = a.sent - a.written + static_cast<uint32_t>(window);
    for (uint32_t f = 0; f < frames && !cancelled(); ++f) {
        s.setPads(PadState{}, PadState{});
        s.runFrame();
        s.audio().clearSamples();
        keyOns(s.apu(), seen, keys, a.written + f);
        if (spcs) spcs->push_back(s.musicSnapshot({}, 0));
    }
    s.wramWatch = nullptr;
}

bool SnesRipper::measure(const std::vector<uint8_t>& spc, double* seconds, uint64_t* identity, std::string* log,
                         std::set<uint32_t>* instruments) {
    snes::Apu apu;
    apu.setMasterRate(1024000.0);  // a clock of the main processor's is one of the SPC700's
    if (!apu.loadSpc(spc)) return false;
    struct Note {
        double t;
        uint32_t token;  // voice, sample and pitch
    };
    std::vector<Note> notes;
    std::array<uint32_t, 8> seen{};
    for (int i = 0; i < 8; ++i) seen[static_cast<size_t>(i)] = apu.dsp().keyOns(i);
    std::vector<int16_t> sink;
    double loopStart = -1, loopLength = 0, end = -1, lastNote = 0;
    constexpr int kTick = 32 * 64;  // 2 ms of the sound processor
    const double longest = std::min(options_.maxSeconds, 240.0);
    for (int tick = 0; tick < static_cast<int>(longest * 500) && !cancelled(); ++tick) {
        apu.addTime(kTick);
        apu.catchUp();
        apu.takeSamples(sink);
        sink.clear();
        const double t = tick / 500.0;
        snes::Dsp& d = apu.dsp();
        for (int i = 0; i < 8; ++i) {
            const uint32_t k = d.keyOns(i);
            if (k == seen[static_cast<size_t>(i)]) continue;
            seen[static_cast<size_t>(i)] = k;
            if (instruments) {
                SynthMusic::VoiceState state;
                apu.dsp().musicVoice(i, state);
                if (state.fingerprint) instruments->insert(state.fingerprint);
            }
            const uint32_t pitch = (d.read(static_cast<uint8_t>(i * 16 + 2)) |
                                    (d.read(static_cast<uint8_t>(i * 16 + 3)) & 0x3F) << 8) >> 6;
            notes.push_back({t, static_cast<uint32_t>(i) << 24 | static_cast<uint32_t>(d.read(static_cast<uint8_t>(i * 16 + 4))) << 16 | pitch});
            lastNote = t;
        }
        // Silent for long: it ended.
        if (!notes.empty() && t - lastNote > 8) {
            end = lastNote + 2;
            break;
        }
        // Every ten seconds past the first forty: does it go round? A turn is a number of
        // notes the last ones repeat, over the last twenty seconds at least (a riff
        // repeated is no loop yet).
        if (tick % 5000 != 4999 || t < 40) continue;
        const size_t n = notes.size();
        for (size_t p = 8; p * 2 <= n; ++p) {
            size_t recent = n;
            while (recent > 0 && notes[recent - 1].t >= notes[n - 1].t - 20) --recent;
            const size_t span = std::max<size_t>({p, 48, n - recent});
            if (span + p > n) break;
            bool same = true;
            for (size_t i = n - span; i < n && same; ++i) same = notes[i].token == notes[i - p].token;
            if (!same) continue;
            size_t first = n - span;
            while (first > p && notes[first - 1].token == notes[first - 1 - p].token) --first;
            loopStart = notes[first - p].t;
            loopLength = notes[n - 1].t - notes[n - 1 - p].t;
            break;
        }
        if (loopLength > 1) break;
    }
    if (notes.size() < 16) return false;
    // A short loop goes round for a minute at least.
    *seconds = loopLength > 1 ? loopStart + loopLength * std::max(2.0, std::ceil(60 / loopLength))
               : end > 0      ? end
                              : longest;
    // What it is, wherever it was started: the notes (not their voices) of a turn of its
    // loop, in any order, or of all of it.
    std::vector<uint32_t> body;
    for (const Note& x : notes)
        if (loopLength <= 1 || x.t >= notes.back().t - loopLength + 0.001) body.push_back(x.token & 0xFFFFFF);
    std::sort(body.begin(), body.end());
    uint64_t h = 1469598103934665603ull;
    for (uint32_t x : body) h = (h ^ x) * 1099511628211ull;
    *identity = h;
    if (log) {
        char line[128];
        std::snprintf(line, sizeof line, "%zu notes, intro %.1f s, loop %.1f s, end %.1f s", notes.size(), loopStart,
                      loopLength, end);
        *log = line;
    }
    return true;
}

bool SnesRipper::run(std::vector<SongRipper::Song>* songs, std::string* error, std::string* report,
                     std::set<uint32_t>* effects) {
    const bool debug = std::getenv("PLUMNIN_RIP_DEBUG") != nullptr;
    std::string log;
    snes_ = std::make_unique<Snes>();
    if (!snes_->loadRomData(rom_, error)) return false;
    snes_->setSampleRate(44100);
    snes_->setDrawing(false);
    if (progress_) progress_->steps = 3 + kMaxTests;

    // 1. The game for a while: what it sends the sound processor, and from where.
    trace();
    step();
    if (cancelled()) return false;

    // 2. The bytes that, written otherwise, start other music. Those sent to the ports
    // first; only if none, those read before a song was loaded (a byte the game reads then
    // may be anything, and set otherwise make it load anything).
    std::set<uint32_t> ways;
    for (int pass = 0; pass < 2 && ways.empty(); ++pass)
    for (const Asked& a : asked_) {
        if (cancelled()) return false;
        if (a.poke != (pass == 1)) continue;
        step();
        if (ways.count(a.index)) continue;
        std::vector<KeyOn> k0, k1;
        listen(a, -1, &k0, nullptr);
        const Heard before = judge(k0, a.sent, a.sent + kWindow);
        const int other = a.value > 1 ? a.value - 1 : a.value + 1;
        listen(a, other, &k1, nullptr);
        const Heard after = judge(k1, a.sent, a.sent + kWindow);
        const bool way = after.music() && (!before.music() || after.differs(before) >= 3);
        if (debug)
            std::fprintf(stderr, "%05X = %02X (written %u, sent %u): %d notes, music %d | %02X: %d notes, music %d%s\n",
                         a.index, a.value, a.written, a.sent, before.notes, before.music(), other, after.notes,
                         after.music(), way ? ", asks for songs" : "");
        if (way) ways.insert(a.index);
    }
    // The other bytes the game sends that add a few notes to what plays, with no music of
    // their own: those it asks for its sound effects by (Super Mario World's 1DF9, 1DFA
    // and 1DFC). As plumsg does, what each number of theirs adds to the music is heard
    // later: the instruments of those sounds that no song plays are the sound effects.
    std::vector<const Asked*> effectWays;
    {
        std::set<uint32_t> tried;
        for (const Asked& a : asked_) {
            if (cancelled()) return false;
            if (a.poke || ways.count(a.index) || !tried.insert(a.index).second || effectWays.size() >= kMaxEffectWays)
                continue;
            std::vector<KeyOn> quiet, own;
            listen(a, 0, &quiet, nullptr, kEffectWindow);
            listen(a, -1, &own, nullptr, kEffectWindow);
            const Heard h = judge(own, a.sent, a.sent + kEffectWindow), q = judge(quiet, a.sent, a.sent + kEffectWindow);
            const bool effect = !added(own, quiet, a.sent).empty() && h.differs(q) <= 2;
            if (debug)
                std::fprintf(stderr, "%05X = %02X: %zu instruments added%s\n", a.index, a.value, added(own, quiet, a.sent).size(),
                             effect ? ", asks for sound effects" : "");
            if (effect) effectWays.push_back(&a);
        }
    }

    // 3. Every number, from each moment the game asked that way.
    struct Found {
        const Asked* from;
        int number;
    };
    std::vector<Found> found;
    std::set<uint64_t> signatures;
    std::vector<const Asked*> starts;
    std::map<uint32_t, int> perWay;
    for (const Asked& a : asked_)
        if (ways.count(a.index) && perWay[a.index]++ < kMaxStarts) starts.push_back(&a);
    if (progress_) progress_->steps = progress_->step + static_cast<int>(starts.size()) * 2 + 8;
    for (const Asked* a : starts) {
        step();
        std::vector<KeyOn> k0;
        listen(*a, -1, &k0, nullptr);
        const Heard before = judge(k0, a->sent, a->sent + kWindow);
        if (before.music() && signatures.insert(before.signature).second) found.push_back({a, a->value});
        int dead = 0;
        for (int n = 0; n < 256 && dead < kDeadNumbers && !cancelled(); ++n) {
            std::vector<KeyOn> k;
            listen(*a, n, &k, nullptr);
            const Heard h = judge(k, a->sent, a->sent + kWindow);
            if (!h.music() || (before.music() && h.differs(before) < 3)) {
                if (!found.empty()) ++dead;
                continue;
            }
            dead = 0;
            if (!signatures.insert(h.signature).second) continue;
            found.push_back({a, n});
            if (progress_) ++progress_->found;
            if (debug) std::fprintf(stderr, "  %05X = %d: %d notes on %d voices\n", a->index, n, h.notes, h.voices);
        }
    }
    if (progress_) progress_->steps = progress_->step + static_cast<int>(found.size() + effectWays.size()) + 1;

    // 4. Each one as an SPC from the moment it starts, its length that of its loop.
    std::set<uint64_t> identities;
    std::map<uint32_t, int> music;  // the instruments the songs play, and in how many
    if (progress_) progress_->found = 0;
    // A song kept, unless heard before.
    const auto keep = [&](const std::vector<uint8_t>& spc, int number, const std::string& where) {
        double seconds = 0;
        uint64_t identity = 0;
        std::string how;
        std::set<uint32_t> instruments;
        if (!measure(spc, &seconds, &identity, &how, &instruments)) return;
        if (!identities.insert(identity).second) {
            if (debug) std::fprintf(stderr, "  %s: heard before\n", where.c_str());
            return;
        }
        for (uint32_t fp : instruments) ++music[fp];
        snes::Apu apu;
        apu.setMasterRate(1024000.0);
        apu.loadSpc(spc);
        SongRipper::Song song;
        song.number = number;
        song.spc = true;
        song.vgz = apu.saveSpc(trf("Canción %02d", static_cast<int>(songs->size()) + 1), snes_->title(),
                               static_cast<int>(std::lround(seconds)));
        song.seconds = seconds;
        songs->push_back(std::move(song));
        if (progress_) ++progress_->found;
        char line[200];
        std::snprintf(line, sizeof line, "Canción %02d: %s, %s\n", static_cast<int>(songs->size()), where.c_str(), how.c_str());
        log += line;
        if (debug) std::fprintf(stderr, "%s", line);
    };
    // No way of asking for them found (a game that loads each song into the sound
    // processor as it needs it, and asks for it otherwise): those the game played itself.
    if (ways.empty()) {
        for (size_t i = 0; i < onsets_.size() && !cancelled(); ++i)
            keep(onsets_[i], -1, trf("la que sonó en el juego (%zu)", i + 1));
        if (songs->empty()) {
            if (error) *error = tr("No se ha encontrado cómo pide el juego sus canciones.");
            return false;
        }
    }
    for (const Found& f : found) {
        if (cancelled()) return false;
        step();
        std::vector<KeyOn> keys;
        std::vector<std::vector<uint8_t>> spcs;
        listen(*f.from, f.number, &keys, &spcs);
        long onset = -1;
        for (const KeyOn& k : keys)
            if (k.frame >= f.from->sent) {
                onset = static_cast<long>(k.frame);
                break;
            }
        const long at = onset - static_cast<long>(f.from->written) - 1;  // the frame before
        if (onset < 0 || at < 0 || at >= static_cast<long>(spcs.size())) continue;
        char where[64];
        std::snprintf(where, sizeof where, "número %d en %05X", f.number, f.from->index);
        keep(spcs[static_cast<size_t>(at)], f.number, where);
    }
    // 5. The sound effects: every number of the ways the game asks for them, over what
    // plays anyway (most drivers play their effects over the music, taking a voice or
    // two of it for a moment). The instruments they add that no song plays.
    if (!songs->empty() && !effectWays.empty() && effects) {
        std::map<uint32_t, int> sounds;  // the instruments the effects play, and in how many
        int numbers = 0;
        for (const Asked* a : effectWays) {
            std::vector<KeyOn> quiet;
            listen(*a, 0, &quiet, nullptr, kEffectWindow);
            int dead = 0, heard = 0;
            for (int n = 1; n < kEffectNumbers && dead < kDeadEffects && !cancelled(); ++n) {
                std::vector<KeyOn> k;
                listen(*a, n, &k, nullptr, kEffectWindow);
                const std::set<uint32_t> its = added(k, quiet, a->sent);
                // (Not what starts other music: a song, or a number past the effects' table.)
                const Heard h = judge(k, a->sent, a->sent + kEffectWindow), q = judge(quiet, a->sent, a->sent + kEffectWindow);
                if (its.empty() || h.differs(q) > 2) {
                    if (heard) ++dead;
                    continue;
                }
                dead = 0;
                ++heard;
                ++numbers;
                for (uint32_t fp : its) ++sounds[fp];
            }
            step();
        }
        // Those no song plays; and those of many effects that only one or two songs play: a
        // driver's effects may share their sample and envelope with the odd jingle (Super
        // Mario World's jump, with a fanfare's drum roll), and an effect taken for a drum
        // is worse than a jingle's notes left as the chip plays them.
        for (const auto& [fp, n] : sounds) {
            const auto m = music.find(fp);
            if (m == music.end() || (m->second <= kFewSongs && n > 2 * m->second)) effects->insert(fp);
        }
        char line[160];
        std::snprintf(line, sizeof line, "Efectos: %d números en %zu bytes, %zu instrumentos, %zu solo en ellos\n", numbers,
                      effectWays.size(), sounds.size(), effects->size());
        log += line;
        if (debug) std::fprintf(stderr, "%s", line);
    }
    if (report) *report = log;
    if (songs->empty() && error) *error = tr("No se ha encontrado cómo pide el juego sus canciones.");
    return !songs->empty();
}

}  // namespace

bool ripSnesSongs(const std::vector<uint8_t>& rom, const SongRipper::Options& options, SongRipper::Progress* progress,
                  std::vector<SongRipper::Song>* songs, std::string* error, std::string* report,
                  std::set<uint32_t>* effects) {
    SnesRipper r(rom, options, progress);
    return r.run(songs, error, report, effects);
}

}  // namespace plumnin
