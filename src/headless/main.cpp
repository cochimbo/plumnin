// plumnin-headless: the emulator without a window, sound card or GPU, for tests, CI and
// reproducing bugs. It runs a cartridge for a number of frames, feeds the
// pads from an input script and writes what was asked for: screenshots, the sound as
// WAV, a hash of every frame, savestates and memory dumps. It can also check the
// result against expected hashes and say on which frame a run first went different.
//
// Run it without arguments (or with --help) for the options and the script format.

#include <cctype>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "cheats.h"
#include "console.h"
#include "miniz.h"
#include "net/udp.h"
#include "netplay.h"
#include "recording.h"
#include "system.h"
#include "test_console.h"

using namespace plumnin;

namespace {

const char* kUsage = R"(usage: plumnin-headless <game|--test-rom> [options]

  game                  cartridge (.gb .gbc .nes .sfc .smc, or inside a .zip)
  --test-rom            no game: the built-in test pattern (two planes, a sprite the
                        pad moves, a tone while A is held), for checking everything
                        around the consoles
  --system gb|gbc|nes|snes   with --test-rom: the console it looks like (default nes)

Running
  --frames N            frames to run (default 600); frames are numbered from 0
  --region jp|us|eu     force the console's region (default: what the game says)
  --no-pad2             nothing plugged into the second port (the script's p2 buttons do nothing)
  --no-sprite-limit     draw every sprite on a line (the option against flicker)
  --widescreen W:H      draw the picture W:H wide (16:9, 16:10) instead of 4:3
  --widescreen L,R      or with L more columns on the left and R on the right (up to 56)
  --overclock PERCENT   run the processor at 100-400% of its speed (default 100)
  --load-state FILE     start from this savestate (after loading the game)
  --cheat CODE          the console's Game Genie or Pro Action Replay code, several
                        joined with + (can repeat)

Input
  --input FILE          input script (see below)
  --do "LINE"           one script line given on the command line (can repeat)

Netplay (two plumnin over UDP, as the window's Red menu does)
  --netplay-host PORT   wait for the other player on this UDP port; this side is pad 1
  --netplay-join ADDR   join the host at ADDR (name or IP, :PORT, default 7845); pad 2
  --input-delay N       frames between a press and its effect (host's choice, default 2)
  --net-lag MS          hold each packet this side sends for MS milliseconds, and
  --net-loss PERCENT    drop this many of them, to try a slow or bad connection
  With either, the script's buttons for this side's pad drive it (p1.* on the host,
  p2.* on the guest) and only button lines are allowed. A button on frame N takes
  effect on frame N as long as N is not below the input delay (those frames have no
  input), so both sides' scripts together, run without netplay, give the same
  frames: compare them with --hash-log and --expect-log. The game starts from the
  host's console when the guest arrives; the log has the frames both agreed on.

Output
  --png FILE            the last frame as PNG
  --layers PREFIX       also PREFIX_planeB.png, PREFIX_planeA.png, PREFIX_sprites.png
  --png-every N         a PNG every N frames, as frame_000123.png in --out-dir
  --out-dir DIR         where screenshots without a name go (default: .)
  --wav FILE            the whole run's sound (44100 Hz stereo)
  --gif FILE            the run as an animated GIF, as plumnin's Grabar GIF makes it
  --avi FILE            the run as a video with its sound (Motion JPEG and PCM)
                        (both at the console's own size, and not with netplay)
  --save-state FILE     a savestate after the last frame
  --dump-ram FILE       the memory RetroAchievements reads (the console's RAM) after
                        the last frame
  --hash-log FILE       one line per frame: frame, picture hash, sound hash
  --json FILE           the summary as JSON (- for standard output)
  --quiet               no summary on standard output

Checks (exit code 3 when one fails)
  --expect-hash HEX     the last frame's picture hash must be HEX
  --expect-audio HEX    the hash of the whole run's sound must be HEX
  --expect-log FILE     every frame must match this --hash-log; the first frame that
                        does not is reported

Input scripts: one command per line, # starts a comment.
  FRAMES BUTTONS...     hold the buttons during FRAMES; BUTTONS are up down left right
                        a b x y l r select start, for pad 2 written p2.a, p2.start...
  FRAMES screenshot [FILE]   PNG after the frame (frame_000123.png in --out-dir)
  FRAMES savestate FILE      savestate after the frame
  FRAMES loadstate FILE      load a savestate before the frame
  FRAMES dump-ram FILE       work RAM after the frame
  FRAMES hash                print the frame's hash
  FRAMES reset               press the reset button before the frame
  FRAMES power               switch off and on again before the frame
  FRAMES quit                stop after this frame
FRAMES is N (one frame), N-M (M included) or N+K (K frames from N). Lines with
buttons add up; a button is held only on the frames its lines cover.

  # wait for the title, press Start, walk right while jumping
  120+4    start
  300-420  right
  330+10   right a
  420      screenshot level.png
  600      quit

Exit codes: 0 fine, 1 could not load or write something, 2 bad arguments or
script, 3 a check failed.
)";

uint32_t fnvAdd(uint32_t h, uint32_t v) {
    h ^= v;
    return h * 16777619u;
}

// The same hash the tests and rom_runner print: FNV-1a over the visible pixels.
uint32_t frameHash(const VideoFrame& f) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) h = fnvAdd(h, f.pixel(x, y));
    return h;
}

uint32_t soundHash(uint32_t h, const std::vector<int16_t>& samples) {
    for (int16_t s : samples) h = fnvAdd(h, static_cast<uint16_t>(s));
    return h;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writePng(const std::string& path, const uint32_t* argb, int width, int height, int stride) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint32_t p = argb[y * stride + x];
            uint8_t* o = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            o[0] = static_cast<uint8_t>(p >> 16);
            o[1] = static_cast<uint8_t>(p >> 8);
            o[2] = static_cast<uint8_t>(p);
            o[3] = 0xFF;
        }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), width, height, 4, &size);
    if (!png) return false;
    const bool ok = writeFile(path, png, size);
    mz_free(png);
    return ok;
}

bool writeWav(const std::string& path, const std::vector<int16_t>& samples, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
    return static_cast<bool>(out);
}

std::string hex8(uint32_t v) {
    char s[16];
    std::snprintf(s, sizeof s, "%08x", v);
    return s;
}

std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", c);
            o += u;
        } else o += c;
    }
    return o + "\"";
}

// What a script line does on its frames.
struct Command {
    enum Kind { Buttons, Screenshot, SaveState, LoadState, DumpRam, Hash, Reset, Power, Quit } kind = Buttons;
    long first = 0, last = 0;  // frames, both included
    PadState pad[2];
    std::string file;
    int line = 0;
};

bool parseFrames(const std::string& s, long* first, long* last) {
    char* end = nullptr;
    const long a = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || a < 0) return false;
    if (*end == 0) {
        *first = *last = a;
        return true;
    }
    const char sep = *end;
    const char* rest = end + 1;
    const long b = std::strtol(rest, &end, 10);
    if (end == rest || *end != 0) return false;
    if (sep == '-' && b >= a) *first = a, *last = b;
    else if (sep == '+' && b > 0) *first = a, *last = a + b - 1;
    else return false;
    return true;
}

bool setButton(PadState& p, const std::string& name) {
    static const std::map<std::string, bool PadState::*> kButtons = {
        {"up", &PadState::up}, {"down", &PadState::down}, {"left", &PadState::left},     {"right", &PadState::right},
        {"a", &PadState::a},   {"b", &PadState::b},       {"x", &PadState::x},           {"y", &PadState::y},
        {"l", &PadState::l},   {"r", &PadState::r},       {"select", &PadState::select}, {"start", &PadState::start},
    };
    const auto it = kButtons.find(name);
    if (it == kButtons.end()) return false;
    p.*(it->second) = true;
    return true;
}

// Parses one script line into `out` (nothing for blank and comment lines).
bool parseLine(const std::string& raw, int lineNo, const std::string& where, std::vector<Command>& out) {
    std::string text = raw.substr(0, raw.find('#'));
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    if (words.empty()) return true;
    auto fail = [&](const std::string& why) {
        std::fprintf(stderr, "%s:%d: %s\n", where.c_str(), lineNo, why.c_str());
        return false;
    };
    Command c;
    c.line = lineNo;
    if (!parseFrames(words[0], &c.first, &c.last)) return fail("bad frames '" + words[0] + "' (N, N-M or N+K)");
    if (words.size() < 2) return fail("nothing to do");
    std::string verb = words[1];
    for (char& ch : verb) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const std::map<std::string, Command::Kind> kVerbs = {
        {"screenshot", Command::Screenshot}, {"savestate", Command::SaveState}, {"loadstate", Command::LoadState},
        {"dump-ram", Command::DumpRam},      {"hash", Command::Hash},           {"reset", Command::Reset},
        {"power", Command::Power},           {"quit", Command::Quit},
    };
    const auto v = kVerbs.find(verb);
    if (v != kVerbs.end()) {
        c.kind = v->second;
        const bool needsFile = c.kind == Command::SaveState || c.kind == Command::LoadState || c.kind == Command::DumpRam;
        const bool takesFile = needsFile || c.kind == Command::Screenshot;
        if (words.size() > (takesFile ? 3u : 2u)) return fail("too many words for " + verb);
        if (needsFile && words.size() < 3) return fail(verb + " needs a file");
        if (words.size() == 3) c.file = words[2];
        out.push_back(c);
        return true;
    }
    for (size_t i = 1; i < words.size(); ++i) {
        std::string b = words[i];
        for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        int port = 0;
        if (b.rfind("p1.", 0) == 0) b = b.substr(3);
        else if (b.rfind("p2.", 0) == 0) b = b.substr(3), port = 1;
        if (!setButton(c.pad[port], b)) return fail("unknown button or command '" + words[i] + "'");
    }
    out.push_back(c);
    return true;
}

void orPad(PadState& d, const PadState& s) {
    d.up |= s.up, d.down |= s.down, d.left |= s.left, d.right |= s.right;
    d.a |= s.a, d.b |= s.b, d.x |= s.x, d.y |= s.y;
    d.l |= s.l, d.r |= s.r, d.select |= s.select, d.start |= s.start;
}

// A slower, lossier network than the real one, in front of the socket: what this side
// sends waits `lagMs` and some packets never leave.
class LaggyTransport : public NetTransport {
public:
    LaggyTransport(NetTransport& inner, long lagMs, long lossPercent)
        : inner_(inner), lag_(static_cast<uint32_t>(lagMs)), loss_(static_cast<uint32_t>(lossPercent)) {}
    void setNow(uint32_t now) {
        now_ = now;
        while (!queue_.empty() && queue_.front().first <= now_) {
            inner_.send(queue_.front().second.data(), queue_.front().second.size());
            queue_.pop_front();
        }
    }
    void send(const uint8_t* data, size_t size) override {
        seed_ = seed_ * 1103515245u + 12345u;
        if ((seed_ >> 16) % 100 < loss_) return;
        queue_.emplace_back(now_ + lag_, std::vector<uint8_t>(data, data + size));
        setNow(now_);
    }
    bool receive(std::vector<uint8_t>& packet) override { return inner_.receive(packet); }
    void keepPeer() override { inner_.keepPeer(); }

private:
    NetTransport& inner_;
    uint32_t lag_, loss_;
    uint32_t now_ = 0;
    uint32_t seed_ = 1;
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> queue_;
};

struct LogLine {
    long frame;
    uint32_t picture, sound;
};

bool readLog(const std::string& path, std::vector<LogLine>& log) {
    std::ifstream in(path);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        LogLine l{};
        unsigned long p = 0, s = 0;
        if (std::sscanf(line.c_str(), "%ld %lx %lx", &l.frame, &p, &s) < 2) continue;
        l.picture = static_cast<uint32_t>(p);
        l.sound = static_cast<uint32_t>(s);
        log.push_back(l);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string game, png, layers, wav, gifOut, aviOut, stateIn, stateOut, ramOut, hashLog, json, expectLog;
    std::string outDir = ".";
    std::vector<std::string> cheats, scripts, lines;
    ConsoleSystem testSystem = ConsoleSystem::Nes;
    long frames = 600, overclock = 100, pngEvery = 0, netPort = 0, inputDelay = 2, netLag = 0, netLoss = 0;
    bool netHost = false;
    std::string netJoin;
    int wideLeft = 0, wideRight = 0;
    bool noPad2 = false, noSpriteLimit = false, quiet = false, haveExpectHash = false, haveExpectAudio = false;
    uint32_t expectHash = 0, expectAudio = 0;
    Region region = Region::Auto;

    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        bool missing = false;
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            missing = true;
            return "";
        };
        auto number = [&](long* out) {
            const std::string s = next();
            char* end = nullptr;
            *out = std::strtol(s.c_str(), &end, 10);
            if (s.empty() || *end != 0 || *out < 0) missing = true;
        };
        auto hash = [&](uint32_t* out) {
            const std::string s = next();
            char* end = nullptr;
            *out = static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 16));
            if (s.empty() || *end != 0) missing = true;
        };
        if (a == "--help" || a == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--frames") number(&frames);
        else if (a == "--region") {
            const std::string r = next();
            if (r == "jp" || r == "japan") region = Region::JapanNtsc;
            else if (r == "us" || r == "usa") region = Region::UsaNtsc;
            else if (r == "eu" || r == "europe" || r == "pal") region = Region::EuropePal;
            else missing = true;
        } else if (a == "--pal") region = Region::EuropePal;
        else if (a == "--system") {
            const std::string v = next();
            if (v == "gb") testSystem = ConsoleSystem::GameBoy;
            else if (v == "gbc") testSystem = ConsoleSystem::GameBoyColor;
            else if (v == "nes") testSystem = ConsoleSystem::Nes;
            else if (v == "snes") testSystem = ConsoleSystem::Snes;
            else missing = true;
        }
        else if (a == "--no-pad2") noPad2 = true;
        else if (a == "--no-sprite-limit") noSpriteLimit = true;
        else if (a == "--widescreen") {
            const std::string v = next();
            int w = 0, h = 0;
            if (std::sscanf(v.c_str(), "%d:%d", &w, &h) == 2 && w > 0 && h > 0) wideLeft = wideRight = wideColumnsFor(w, h);
            else if (std::sscanf(v.c_str(), "%d,%d", &wideLeft, &wideRight) != 2 || wideLeft < 0 || wideRight < 0) missing = true;
        }
        else if (a == "--overclock") number(&overclock);
        else if (a == "--load-state") stateIn = next();
        else if (a == "--cheat") cheats.push_back(next());
        else if (a == "--input") scripts.push_back(next());
        else if (a == "--do") lines.push_back(next());
        else if (a == "--png") png = next();
        else if (a == "--layers") layers = next();
        else if (a == "--png-every") number(&pngEvery);
        else if (a == "--out-dir") outDir = next();
        else if (a == "--wav") wav = next();
        else if (a == "--gif") gifOut = next();
        else if (a == "--avi") aviOut = next();
        else if (a == "--save-state") stateOut = next();
        else if (a == "--dump-ram") ramOut = next();
        else if (a == "--hash-log") hashLog = next();
        else if (a == "--json") json = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--expect-hash") hash(&expectHash), haveExpectHash = true;
        else if (a == "--expect-audio") hash(&expectAudio), haveExpectAudio = true;
        else if (a == "--expect-log") expectLog = next();
        else if (a == "--netplay-host") {
            number(&netPort);
            if (netPort == 0 || netPort > 65535) missing = true;
            netHost = true;
        } else if (a == "--netplay-join") netJoin = next();
        else if (a == "--net-lag") number(&netLag);
        else if (a == "--net-loss") {
            number(&netLoss);
            if (netLoss > 90) missing = true;
        } else if (a == "--input-delay") {
            number(&inputDelay);
            if (inputDelay > 10) missing = true;
        }
        else if (a.size() > 2 && a.compare(0, 2, "--") == 0 && a != "--test-rom") {
            std::fprintf(stderr, "unknown option %s (see --help)\n", a.c_str());
            return 2;
        } else if (!game.empty()) {
            std::fprintf(stderr, "only one game at a time (%s and %s)\n", game.c_str(), a.c_str());
            return 2;
        } else game = a;
        if (missing) {
            std::fprintf(stderr, "%s needs a valid value (see --help)\n", a.c_str());
            return 2;
        }
    }
    if (game.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const bool netplay = netHost || !netJoin.empty();
    if (netHost && !netJoin.empty()) {
        std::fprintf(stderr, "--netplay-host and --netplay-join: one side or the other\n");
        return 2;
    }

    // The script.
    std::vector<Command> commands;
    for (const std::string& path : scripts) {
        std::ifstream in(path);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        int n = 0;
        for (std::string line; std::getline(in, line);)
            if (!parseLine(line, ++n, path, commands)) return 2;
    }
    for (size_t i = 0; i < lines.size(); ++i)
        if (!parseLine(lines[i], static_cast<int>(i + 1), "--do", commands)) return 2;

    if (netplay)
        for (const Command& c : commands)
            if (c.kind != Command::Buttons) {
                std::fprintf(stderr, "script line %d: only buttons can be scripted in netplay\n", c.line);
                return 2;
            }

    std::vector<LogLine> expected;
    if (!expectLog.empty() && !readLog(expectLog, expected)) {
        std::fprintf(stderr, "cannot read %s\n", expectLog.c_str());
        return 1;
    }

    // The console.
    std::unique_ptr<Console> console;
    if (game == "--test-rom") {
        console = std::make_unique<TestConsole>(testSystem);
        console->loadRomData({0});
    } else {
        std::string error;
        console = openGame(game, &error);
        if (!console) {
            std::fprintf(stderr, "cannot load %s: %s\n", game.c_str(), error.c_str());
            return 1;
        }
        if (!console->loadNotice().empty()) std::fprintf(stderr, "%s\n", console->loadNotice().c_str());
    }
    Console& md = *console;
    md.setRegion(region);
    if (noPad2 && !netplay) md.setPadType(1, PadType::None);  // the guest plays on pad 2
    md.setUnlimitedSprites(noSpriteLimit);
    md.setWidescreen(wideLeft, wideRight);
    md.setOverclock(static_cast<int>(overclock));
    if (!cheats.empty()) {
        std::vector<CheatPatch> patches;
        for (const std::string& code : cheats) {
            std::vector<CheatPatch> p;
            std::string error;
            if (!decodeCheatCodes(code, md.cheatDecoder(), &p, &error)) {
                std::fprintf(stderr, "bad cheat %s: %s\n", code.c_str(), error.c_str());
                return 2;
            }
            patches.insert(patches.end(), p.begin(), p.end());
        }
        md.setCheats(patches);
    }
    auto loadState = [&](const std::string& path) {
        std::vector<uint8_t> data;
        std::string error;
        if (!readFile(path, data)) error = "cannot read it";
        else if (md.loadState(data, &error)) return true;
        std::fprintf(stderr, "cannot load state %s: %s\n", path.c_str(), error.c_str());
        return false;
    };
    auto saveState = [&](const std::string& path) {
        std::vector<uint8_t> data;
        if (md.saveState(data) && writeFile(path, data.data(), data.size())) return true;
        std::fprintf(stderr, "cannot write state %s\n", path.c_str());
        return false;
    };
    auto dumpRam = [&](const std::string& path) {
        std::vector<uint8_t> ram;
        for (const MemoryRegion& m : md.memoryMap()) ram.insert(ram.end(), m.data, m.data + m.size);
        if (writeFile(path, ram.data(), ram.size())) return true;
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    };
    auto screenshot = [&](std::string path, long frame) {
        if (path.empty()) {
            char name[32];
            std::snprintf(name, sizeof name, "frame_%06ld.png", frame);
            path = outDir + "/" + name;
        }
        const VideoFrame& f = md.frame();
        if (writePng(path, f.rgba.data(), f.width, f.height, VideoFrame::kMaxWidth)) return true;
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    };
    if (!stateIn.empty() && !loadState(stateIn)) return 1;

    std::FILE* log = nullptr;
    if (!hashLog.empty()) {
        log = std::fopen(hashLog.c_str(), "w");
        if (!log) {
            std::fprintf(stderr, "cannot write %s\n", hashLog.c_str());
            return 1;
        }
        std::fprintf(log, "# frame picture sound\n");
    }

    constexpr int kRate = 44100;
    md.setSampleRate(kRate);
    md.audio().clearSamples();
    std::vector<int16_t> sound;
    uint32_t audioHash = 2166136261u;
    long divergence = -1;
    std::string divergenceWhat;
    size_t nextExpected = 0;
    long ran = 0;
    bool ioError = false;
    // --gif and --avi: started on the first frame, which gives them their size.
    GameRecording gifRecording, aviRecording;
    auto recordVideo = [&](GameRecording& r, const std::string& path, GameRecording::Format format) {
        if (path.empty()) return;
        const VideoFrame& f = md.frame();
        std::string error;
        // As wide and as tall as the first frame, without interlace.
        const int height = f.interlaced ? f.height / 2 : f.height;
        if (!r.active() && !r.start(path, format, f.width, height, md.refreshRate(), 44100, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            ioError = true;
            return;
        }
        r.addAudio(md.audio().samples().data(), md.audio().samples().size());
        r.addFrame(f.rgba.data(), f.width, f.height, VideoFrame::kMaxWidth);
    };

    // What happens after each frame is final: the log, the expected log, the sound.
    auto record = [&](long frame, uint32_t picture, const std::vector<int16_t>& samples) {
        const uint32_t frameSound = soundHash(2166136261u, samples);
        audioHash = soundHash(audioHash, samples);
        if (!wav.empty()) sound.insert(sound.end(), samples.begin(), samples.end());
        if (log) std::fprintf(log, "%ld %08x %08x\n", frame, picture, frameSound);
        while (nextExpected < expected.size() && expected[nextExpected].frame < frame) ++nextExpected;
        if (divergence < 0 && nextExpected < expected.size() && expected[nextExpected].frame == frame) {
            const LogLine& e = expected[nextExpected];
            if (e.picture != picture) {
                divergence = frame;
                divergenceWhat = "picture " + hex8(picture) + ", expected " + hex8(e.picture);
            } else if (e.sound != frameSound) {
                divergence = frame;
                divergenceWhat = "sound " + hex8(frameSound) + ", expected " + hex8(e.sound);
            }
        }
    };

    const auto start = std::chrono::steady_clock::now();
    std::string netError;
    long rollbacks = 0;
    int ping = 0;
    if (netplay) {
        UdpTransport udp;
        bool ok = udp.open(netHost ? static_cast<int>(netPort) : 0) && (netHost || udp.setPeer(netJoin, 7845));
        if (!ok) netError = udp.error();
        NetplaySession::Config config;
        config.inputDelay = static_cast<int>(inputDelay);
        LaggyTransport link(udp, netLag, netLoss);
        NetplaySession session(link, netHost ? NetplaySession::Role::Host : NetplaySession::Role::Guest, config);
        // A frame can run several times (guessed pads, then the right ones): keep the
        // last run of each, which is final once both sides have its pads.
        std::vector<std::vector<int16_t>> frameSamples(static_cast<size_t>(frames));
        std::vector<uint32_t> framePictures(static_cast<size_t>(frames));
        session.onFrame = [&](uint32_t f, Console& m) {
            if (f >= static_cast<uint32_t>(frames)) return;
            frameSamples[f] = m.audio().samples();
            framePictures[f] = frameHash(m.frame());
        };
        const int port = netHost ? 0 : 1;
        const auto clockStart = std::chrono::steady_clock::now();
        auto nowMs = [&] {
            return static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clockStart).count());
        };
        uint32_t doneAt = 0;
        while (ok) {
            const uint32_t now = nowMs();
            link.setNow(now);
            session.poll(md, now);
            if (session.phase() == NetplaySession::Phase::Ended) {
                netError = session.endReason();
                break;
            }
            if (session.phase() == NetplaySession::Phase::Waiting && now > 60000) {
                netError = "nobody joined within a minute";
                break;
            }
            bool advanced = false;
            if (session.frame() < static_cast<uint32_t>(frames)) {
                const long n = static_cast<long>(session.frame()) + inputDelay;  // the frame this press lands on
                PadState pads[2];
                for (const Command& c : commands)
                    if (n >= c.first && n <= c.last) orPad(pads[0], c.pad[0]), orPad(pads[1], c.pad[1]);
                advanced = session.advance(md, pads[port]);
                rollbacks += session.lastRollback();
                md.audio().clearSamples();
            } else {
                session.catchUp(md);
                if (session.confirmedFrames() >= static_cast<uint32_t>(frames)) {
                    // Linger a moment so the other side gets our last pads too.
                    if (doneAt == 0) doneAt = now;
                    if (now - doneAt > 1000) break;
                }
            }
            if (!advanced) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ping = session.pingMs();
        if (session.desyncFrame() >= 0 && netError.empty())
            netError = "the two consoles went different on frame " + std::to_string(session.desyncFrame());
        session.end();
        if (netError.empty() || session.confirmedFrames() >= static_cast<uint32_t>(frames)) {
            netError.clear();
            // The log and the checks, from each frame's final run. md shows the last one.
            for (long f = 0; f < frames; ++f) {
                record(f, framePictures[f], frameSamples[f]);
                ++ran;
            }
        }
    }
    for (long frame = 0; frame < frames && !netplay; ++frame) {
        bool quit = false;
        PadState pads[2];
        for (const Command& c : commands) {
            if (frame < c.first || frame > c.last) continue;
            switch (c.kind) {
                case Command::Buttons: orPad(pads[0], c.pad[0]), orPad(pads[1], c.pad[1]); break;
                case Command::LoadState:
                    if (!loadState(c.file)) ioError = true;
                    break;
                case Command::Reset: md.reset(); break;
                case Command::Power: md.powerCycle(); break;
                default: break;
            }
        }
        md.setPads(pads[0], pads[1]);
        md.runFrame();
        ++ran;
        const uint32_t picture = frameHash(md.frame());
        record(frame, picture, md.audio().samples());
        if (!netplay) {
            recordVideo(gifRecording, gifOut, GameRecording::Format::Gif);
            recordVideo(aviRecording, aviOut, GameRecording::Format::Avi);
        }
        md.audio().clearSamples();
        if (pngEvery > 0 && frame % pngEvery == 0 && !screenshot("", frame)) ioError = true;
        for (const Command& c : commands) {
            if (frame < c.first || frame > c.last) continue;
            switch (c.kind) {
                case Command::Screenshot: if (!screenshot(c.file, frame)) ioError = true; break;
                case Command::SaveState: if (!saveState(c.file)) ioError = true; break;
                case Command::DumpRam: if (!dumpRam(c.file)) ioError = true; break;
                case Command::Hash: std::printf("frame %ld hash %08x\n", frame, picture); break;
                case Command::Quit: quit = true; break;
                default: break;
            }
        }
        if (quit) break;
    }
    if (!netError.empty()) {
        std::fprintf(stderr, "netplay: %s\n", netError.c_str());
        return 1;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (log) std::fclose(log);
    if (!expected.empty() && divergence < 0 && static_cast<long>(expected.back().frame) >= ran) {
        divergence = ran;
        divergenceWhat = "the run ended before the expected log";
    }

    const VideoFrame& f = md.frame();
    const uint32_t lastHash = frameHash(f);
    if (!png.empty() && !screenshot(png, ran - 1)) ioError = true;
    if (!layers.empty()) {
        for (auto [name, layer] : {std::pair<const char*, const uint32_t*>{"_planeB.png", f.planeB.data()},
                                   {"_planeA.png", f.planeA.data()}, {"_sprites.png", f.sprites.data()}})
            if (!writePng(layers + name, layer, f.width, f.height, VideoFrame::kMaxWidth)) ioError = true;
    }
    if (!wav.empty() && !writeWav(wav, sound, kRate)) ioError = true;
    for (GameRecording* r : {&gifRecording, &aviRecording}) {
        std::string error;
        if (!r->finish(&error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            ioError = true;
        }
    }
    if (!stateOut.empty() && !saveState(stateOut)) ioError = true;
    if (!ramOut.empty() && !dumpRam(ramOut)) ioError = true;

    std::vector<std::string> failures;
    if (haveExpectHash && lastHash != expectHash)
        failures.push_back("picture hash " + hex8(lastHash) + ", expected " + hex8(expectHash));
    if (haveExpectAudio && audioHash != expectAudio)
        failures.push_back("sound hash " + hex8(audioHash) + ", expected " + hex8(expectAudio));
    if (divergence >= 0) failures.push_back("frame " + std::to_string(divergence) + ": " + divergenceWhat);

    const char* system = systemKey(md.system());
    const bool pal = md.effectiveRegion() == Region::EuropePal;
    const double fps = seconds > 0 ? ran / seconds : 0;
    if (!quiet) {
        std::printf("title: %s\nsystem: %s  region: %s\nframes: %ld  size: %dx%d  hash: %08x\n",
                    md.title().c_str(), system, pal ? "pal" : "ntsc", ran, f.width, f.height, lastHash);
        std::printf("audio hash: %08x\n", audioHash);
        std::printf("speed: %.0f fps (%.1fx)\n", fps, fps / md.refreshRate());
        if (netplay) std::printf("netplay: %ld frames run again, ping %d ms\n", rollbacks, ping);
        for (const std::string& s : failures) std::printf("FAILED: %s\n", s.c_str());
    }
    if (!json.empty()) {
        std::ostringstream j;
        j << "{\n  \"title\": " << jsonString(md.title()) << ",\n  \"system\": \"" << system
          << "\",\n  \"pal\": " << (pal ? "true" : "false") << ",\n  \"frames\": " << ran
          << ",\n  \"width\": " << f.width << ",\n  \"height\": " << f.height << ",\n  \"hash\": \"" << hex8(lastHash)
          << "\",\n  \"audio_hash\": \"" << hex8(audioHash) << "\"";
        j << ",\n  \"fps\": " << static_cast<long>(fps) << ",\n  \"divergence\": " << divergence;
        if (netplay) j << ",\n  \"netplay_rollback_frames\": " << rollbacks << ",\n  \"netplay_ping_ms\": " << ping;
        j << ",\n  \"failures\": [";
        for (size_t i = 0; i < failures.size(); ++i) j << (i ? ", " : "") << jsonString(failures[i]);
        j << "]\n}\n";
        if (json == "-") std::fputs(j.str().c_str(), stdout);
        else if (std::ofstream out(json); !(out << j.str())) ioError = true;
    }
    if (ioError) return 1;
    return failures.empty() ? 0 : 3;
}
