// Runs the Game Boy test ROMs (https://github.com/c-sp/game-boy-test-roms) on plumnin's
// Game Boy and says which pass. They are not part of plumnin: download a release and
// give its folder.
//
//   gb-test-roms ROOT              the suite below; exit code 1 if one that should pass fails
//   gb-test-roms ROOT --all        also the ones not expected to pass yet
//   gb-test-roms --run ROM [--frames N] [--png OUT] [--ref PNG]   one ROM, to look at it
//
// A test passes in one of three ways, depending on how it reports:
//   serial     blargg's: "Passed" sent through the link cable (or in the cartridge RAM)
//   mooneye    LD B,B with the Fibonacci numbers 3 5 8 13 21 34 in B C D E H L
//   screen     the picture after a number of frames is the reference PNG

#include <array>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "gb/gb.h"

using plumnin::GameBoy;

namespace {

enum class Kind { Serial, Mooneye, Screen };

struct Test {
    const char* path;
    Kind kind;
    int frames;
    const char* reference = nullptr;  // for Screen, relative to ROOT
    bool expected = true;             // false: known to fail for now
};

const std::array<uint32_t, 4> kGreys{0xFFFFFFFFu, 0xFFAAAAAAu, 0xFF555555u, 0xFF000000u};

struct Result {
    bool pass = false;
    std::string detail;
};

std::unique_ptr<GameBoy> load(const std::string& path, std::string* error) {
    auto gb = std::make_unique<GameBoy>();
    if (!gb->loadRom(path, error)) return nullptr;
    gb->setDmgPalette(kGreys);
    gb->setSampleRate(44100);
    return gb;
}

bool comparePng(const GameBoy& gb, const std::string& ref, std::string* detail) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(ref.c_str(), &w, &h, &n, 3);
    if (!px) {
        *detail = "no reference " + ref;
        return false;
    }
    const auto& f = gb.frame();
    int wrong = 0, firstX = -1, firstY = -1;
    if (w != f.width || h != f.height) {
        *detail = "reference is " + std::to_string(w) + "x" + std::to_string(h);
        stbi_image_free(px);
        return false;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px + (y * w + x) * 3;
            const uint32_t want = static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
            if ((f.pixel(x, y) & 0xFFFFFF) != want) {
                if (!wrong++) firstX = x, firstY = y;
            }
        }
    stbi_image_free(px);
    if (wrong) {
        char b[96];
        std::snprintf(b, sizeof b, "%d pixels differ, first at %d,%d", wrong, firstX, firstY);
        *detail = b;
    }
    return !wrong;
}

void writePng(const GameBoy& gb, const std::string& out) {
    const auto& f = gb.frame();
    std::vector<unsigned char> rgb(static_cast<size_t>(f.width) * f.height * 3);
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            const uint32_t c = f.pixel(x, y);
            unsigned char* p = &rgb[(static_cast<size_t>(y) * f.width + x) * 3];
            p[0] = static_cast<unsigned char>(c >> 16), p[1] = static_cast<unsigned char>(c >> 8),
            p[2] = static_cast<unsigned char>(c);
        }
    stbi_write_png(out.c_str(), f.width, f.height, 3, rgb.data(), f.width * 3);
}

// blargg's tests with cartridge RAM also leave their result there: A001-A003 hold
// DE B0 61 once it is valid, A000 is 80 while running and then the result code.
bool blarggRamDone(const GameBoy& gb, int* code, std::string* text) {
    if (gb.peek(0xA001) != 0xDE || gb.peek(0xA002) != 0xB0 || gb.peek(0xA003) != 0x61) return false;
    const uint8_t status = gb.peek(0xA000);
    if (status == 0x80) return false;
    *code = status;
    text->clear();
    for (uint16_t a = 0xA004; a < 0xB000 && gb.peek(a); ++a) text->push_back(static_cast<char>(gb.peek(a)));
    return true;
}

Result run(const std::string& rom, Kind kind, int frames, const std::string& ref) {
    Result r;
    std::string error;
    auto gb = load(rom, &error);
    if (!gb) {
        r.detail = error;
        return r;
    }
    for (int i = 0; i < frames; ++i) {
        gb->runFrame();
        gb->audio().clearSamples();
        if (kind == Kind::Serial) {
            const std::string& s = gb->serialOutput();
            if (s.find("Passed") != std::string::npos) return {true, ""};
            if (s.find("Failed") != std::string::npos) break;
            int code = 0;
            std::string text;
            if (blarggRamDone(*gb, &code, &text)) {
                if (code == 0) return {true, ""};
                r.detail = text;
                return r;
            }
        } else if (kind == Kind::Mooneye && gb->hitBreakpoint()) {
            // A few more instructions' worth: the test sets the registers just before.
            const auto regs = gb->registers();
            const bool ok = regs[1] == 0x0305 && regs[2] == 0x080D && regs[3] == 0x1522;
            if (!ok) {
                char b[64];
                std::snprintf(b, sizeof b, "BC=%04X DE=%04X HL=%04X", regs[1], regs[2], regs[3]);
                r.detail = b;
            }
            r.pass = ok;
            return r;
        }
    }
    if (kind == Kind::Serial) {
        r.detail = gb->serialOutput();
        while (!r.detail.empty() && (r.detail.back() == '\n' || r.detail.back() == ' ')) r.detail.pop_back();
        for (auto& c : r.detail)
            if (c == '\n') c = ' ';
        if (r.detail.empty()) r.detail = "no result";
    } else if (kind == Kind::Mooneye) {
        r.detail = "never reached LD B,B";
    } else {
        r.pass = comparePng(*gb, ref, &r.detail);
    }
    return r;
}

// clang-format off
const Test kTests[] = {
    {"blargg/cpu_instrs/cpu_instrs.gb", Kind::Serial, 4000},
    {"blargg/instr_timing/instr_timing.gb", Kind::Serial, 300},
    {"blargg/mem_timing/mem_timing.gb", Kind::Serial, 300},
    {"blargg/mem_timing-2/mem_timing.gb", Kind::Serial, 300},
    {"blargg/halt_bug.gb", Kind::Screen, 200, "blargg/halt_bug-dmg-cgb.png"},
    // The wave channel's RAM read and written while it plays (09, 10, 12) is not emulated.
    {"blargg/dmg_sound/dmg_sound.gb", Kind::Serial, 3000, nullptr, false},
    {"blargg/cgb_sound/cgb_sound.gb", Kind::Serial, 3000, nullptr, false},
    {"blargg/interrupt_time/interrupt_time.gb", Kind::Serial, 300, nullptr, false},
    {"dmg-acid2/dmg-acid2.gb", Kind::Screen, 60, "dmg-acid2/dmg-acid2-dmg.png"},
    {"cgb-acid2/cgb-acid2.gbc", Kind::Screen, 60, "cgb-acid2/cgb-acid2.png"},
};

// mooneye's acceptance tests and the cartridge ones, all of them but those for other
// models (boot_* of the dmg0, mgb, sgb and the Color) and these, not passing yet.
const char* const kMooneyeFolders[] = {"mooneye-test-suite/acceptance", "mooneye-test-suite/emulator-only"};
const char* const kMooneyeKnownFailures[] = {
    "acceptance/ppu/intr_2_mode0_timing.gb",
    "acceptance/ppu/intr_2_mode0_timing_sprites.gb",
    "acceptance/ppu/intr_2_mode3_timing.gb",
    "acceptance/ppu/intr_2_oam_ok_timing.gb",
    "acceptance/ppu/lcdon_timing-GS.gb",
    "acceptance/ppu/lcdon_write_timing-GS.gb",
    "acceptance/ppu/stat_lyc_onoff.gb",
    "acceptance/serial/boot_sclk_align-dmgABCmgb.gb",
    "acceptance/timer/rapid_toggle.gb",
    "emulator-only/mbc1/multicart_rom_8Mb.gb",
};
// clang-format on

bool otherModel(const std::string& name) {
    for (const char* m : {"-dmg0", "-mgb", "-sgb", "-S.", "-S-", "-C.", "-A.", "-cgb", "-ags"})
        if (name.find(m) != std::string::npos && name.find("dmgABC") == std::string::npos) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--run") == 0) {
        const std::string rom = argv[2];
        int frames = 600;
        std::string out, ref;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--png")) out = argv[i + 1];
            else if (!std::strcmp(argv[i], "--ref")) ref = argv[i + 1];
        }
        std::string error;
        auto gb = load(rom, &error);
        if (!gb) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        int at = -1;
        for (int i = 0; i < frames; ++i) {
            gb->runFrame();
            gb->audio().clearSamples();
            if (gb->hitBreakpoint() && at < 0) at = i;
        }
        const auto regs = gb->registers();
        std::printf("serial: %s\n", gb->serialOutput().c_str());
        std::printf("AF=%04X BC=%04X DE=%04X HL=%04X  LD B,B at frame %d\n", regs[0], regs[1], regs[2], regs[3], at);
        int code = 0;
        std::string text;
        if (blarggRamDone(*gb, &code, &text)) std::printf("A000 result %d: %s\n", code, text.c_str());
        if (!out.empty()) writePng(*gb, out);
        if (!ref.empty()) {
            std::string detail;
            std::printf("screen: %s %s\n", comparePng(*gb, ref, &detail) ? "same" : "different", detail.c_str());
        }
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: gb-test-roms ROOT [--all] | --run ROM [--frames N] [--png OUT] [--ref PNG]\n");
        return 2;
    }
    const std::string root = std::string(argv[1]) + "/";
    const bool all = argc > 2 && !std::strcmp(argv[2], "--all");
    int failed = 0, passed = 0;
    for (const Test& t : kTests) {
        if (!t.expected && !all) continue;
        const Result r = run(root + t.path, t.kind, t.frames, t.reference ? root + t.reference : "");
        std::printf("%s %s%s%s\n", r.pass ? "ok  " : t.expected ? "FAIL" : "fail", t.path, r.detail.empty() ? "" : "  ", r.detail.c_str());
        if (r.pass) ++passed;
        else if (t.expected) ++failed;
    }
    for (const char* folder : kMooneyeFolders) {
        std::vector<std::string> roms;
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root + folder, ec))
            if (e.path().extension() == ".gb") roms.push_back(e.path().generic_string());
        std::sort(roms.begin(), roms.end());
        for (const std::string& rom : roms) {
            const std::string name = rom.substr(root.size() + std::strlen("mooneye-test-suite/"));
            if (otherModel(name)) continue;
            bool expected = true;
            for (const char* f : kMooneyeKnownFailures) expected = expected && name != f;
            if (!expected && !all) continue;
            const Result r = run(rom, Kind::Mooneye, 600, "");
            std::printf("%s mooneye/%s%s%s\n", r.pass ? "ok  " : expected ? "FAIL" : "fail", name.c_str(),
                        r.detail.empty() ? "" : "  ", r.detail.c_str());
            if (r.pass) ++passed;
            else if (expected) ++failed;
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
