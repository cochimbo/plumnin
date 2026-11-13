// plumnin's core tests: everything around the consoles, driven by the test pattern
// (TestConsole), so that they need no commercial ROM. Each console adds its own tests
// here as it is emulated.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "console.h"
#include "gb/gb.h"
#include "nes/nes.h"
#include "snes/snes.h"
#include "miniz.h"
#include "netplay.h"
#include "render/planes.h"
#include "render/profile3d.h"
#include "rewind.h"
#include "system.h"
#include "test_console.h"
#include "frontend/achievements.h"
#include "frontend/game_info_data.h"
#include "frontend/library_names.h"
#include "frontend/screenscraper_data.h"
#include "frontend/texture_cache.h"

using namespace plumnin;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

uint32_t frameHash(const VideoFrame& f) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            h ^= f.pixel(x, y);
            h *= 16777619u;
        }
    return h;
}

uint32_t soundHash(const std::vector<int16_t>& s) {
    uint32_t h = 2166136261u;
    for (int16_t v : s) {
        h ^= static_cast<uint16_t>(v);
        h *= 16777619u;
    }
    return h;
}

// A test pattern with a "cartridge" in it (any bytes will do).
std::unique_ptr<TestConsole> testConsole(ConsoleSystem s = ConsoleSystem::Nes, uint8_t cartridge = 1) {
    auto c = std::make_unique<TestConsole>(s);
    c->loadRomData(std::vector<uint8_t>(64, cartridge));
    c->setSampleRate(44100);
    return c;
}

bool writeZip(const std::string& path, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    for (const auto& [name, data] : entries) {
        mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                              static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION));
    }
    void* buf = nullptr;
    size_t size = 0;
    const bool ok = mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    if (ok) std::ofstream(path, std::ios::binary).write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return ok;
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& d) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

// The start of a Game Boy cartridge: its logo at 0x104, and the Color flag at 0x143.
std::vector<uint8_t> gameBoyRom(bool colorOnly) {
    static const uint8_t logo[48] = {0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
                                     0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
                                     0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
                                     0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E};
    std::vector<uint8_t> rom(0x8000, 0);
    std::copy(std::begin(logo), std::end(logo), rom.begin() + 0x104);
    rom[0x143] = colorOnly ? 0xC0 : 0x00;
    return rom;
}

// A small Game Boy program, so that the Game Boy can be tested without any game: it
// fills the background with a checkerboard of solid tiles that scrolls one pixel a
// frame, puts a sprite of a solid tile at (0x50, 0x50) that the cross moves, and beeps
// on the first square channel while A is held.
std::vector<uint8_t> gameBoyProgram(uint8_t type = 0x00, uint8_t ramSize = 0x00) {
    std::vector<uint8_t> rom = gameBoyRom(false);
    static const uint8_t code[] = {
        0xF3, 0x31, 0xFE, 0xFF, 0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40, 0x21, 0x10, 0x80,
        0x3E, 0xFF, 0x06, 0x10, 0x22, 0x05, 0x20, 0xFC, 0x21, 0x00, 0x98, 0x06, 0x00, 0x7D, 0xE6, 0x01,
        0x22, 0x05, 0x20, 0xF9, 0x21, 0x00, 0xFE, 0x3E, 0x50, 0x22, 0x22, 0x3E, 0x01, 0x22, 0xAF, 0x77,
        0x3E, 0xE4, 0xE0, 0x47, 0xE0, 0x48, 0x3E, 0x93, 0xE0, 0x40, 0x3E, 0x80, 0xE0, 0x26, 0x3E, 0xFF,
        0xE0, 0x25, 0x3E, 0x77, 0xE0, 0x24, 0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0x3E, 0x20, 0xE0, 0x00,
        0xF0, 0x00, 0xF0, 0x00, 0x2F, 0x47, 0xCB, 0x40, 0x28, 0x04, 0x21, 0x01, 0xFE, 0x34, 0xCB, 0x48,
        0x28, 0x04, 0x21, 0x01, 0xFE, 0x35, 0xCB, 0x50, 0x28, 0x04, 0x21, 0x00, 0xFE, 0x35, 0xCB, 0x58,
        0x28, 0x04, 0x21, 0x00, 0xFE, 0x34, 0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0x2F, 0xCB,
        0x47, 0x28, 0x08, 0x3E, 0xF0, 0xE0, 0x12, 0x3E, 0x87, 0xE0, 0x14, 0xF0, 0x43, 0x3C, 0xE0, 0x43,
        0xF0, 0x44, 0xFE, 0x90, 0x28, 0xFA, 0x18, 0xAE,
    };
    const uint8_t start[] = {0x00, 0xC3, 0x50, 0x01};  // NOP; JP 0150
    std::copy(std::begin(start), std::end(start), rom.begin() + 0x100);
    const char title[] = "PLUMNIN TEST";
    std::copy(title, title + sizeof title - 1, rom.begin() + 0x134);
    rom[0x147] = type;
    rom[0x149] = ramSize;
    std::copy(std::begin(code), std::end(code), rom.begin() + 0x150);
    return rom;
}

// A tiny 6502 assembler, enough for the NES program below: bytes, labels and the
// relative branches to them.
struct Asm6502 {
    std::vector<uint8_t> code;
    std::vector<std::pair<size_t, size_t*>> branches;  // where the offset goes, the label
    uint16_t here() const { return static_cast<uint16_t>(0x8000 + code.size()); }
    void emit(std::initializer_list<int> bytes) {
        for (int b : bytes) code.push_back(static_cast<uint8_t>(b));
    }
    void abs(int op, uint16_t a) { emit({op, a & 0xFF, a >> 8}); }
    void branch(int op, size_t* label) {
        emit({op, 0});
        branches.push_back({code.size() - 1, label});
    }
    void resolve() {
        for (auto& [at, label] : branches) code[at] = static_cast<uint8_t>(*label - (at + 1));
    }
};

// A small NES program (NROM, 16 KB of PRG and 8 KB of CHR), so that the NES can be
// tested without any game: vertical stripes of a solid tile that scroll one pixel a
// frame, a red sprite of the same tile at (0x50, 0x50) that the cross moves, and a
// beep on the first pulse channel while A is held. Its work is done in the NMI.
std::vector<uint8_t> nesProgram(bool battery = false) {
    Asm6502 a;
    size_t vb1, vb2, fill, clear, loop, nmi, noA, noL, noR;
    a.emit({0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x00});  // sei cld ldx #$FF txs lda #0
    a.abs(0x8D, 0x2000);
    a.abs(0x8D, 0x2001);
    vb1 = a.code.size();
    a.abs(0x2C, 0x2002);
    a.branch(0x10, &vb1);
    vb2 = a.code.size();
    a.abs(0x2C, 0x2002);
    a.branch(0x10, &vb2);
    // Palette: black backdrop, white background colour 1, red sprite colour 1.
    auto ppuAddr = [&](uint16_t addr) {
        a.emit({0xA9, addr >> 8});
        a.abs(0x8D, 0x2006);
        a.emit({0xA9, addr & 0xFF});
        a.abs(0x8D, 0x2006);
    };
    auto ppuData = [&](int v) {
        a.emit({0xA9, v});
        a.abs(0x8D, 0x2007);
    };
    ppuAddr(0x3F00);
    ppuData(0x0F);
    ppuData(0x30);
    ppuAddr(0x3F11);
    ppuData(0x16);
    // The nametable and its attributes: tiles 0 1 0 1...
    ppuAddr(0x2000);
    a.emit({0xA0, 0x00, 0xA2, 0x04});  // ldy #0 ldx #4
    fill = a.code.size();
    a.emit({0x98, 0x29, 0x01});  // tya and #1
    a.abs(0x8D, 0x2007);
    a.emit({0xC8});  // iny
    a.branch(0xD0, &fill);
    a.emit({0xCA});  // dex
    a.branch(0xD0, &fill);
    // Sprites at 0200: all hidden but the first.
    a.emit({0xA2, 0x00, 0xA9, 0xFF});
    clear = a.code.size();
    a.abs(0x9D, 0x0200);
    a.emit({0xE8});
    a.branch(0xD0, &clear);
    a.emit({0xA9, 0x50});
    a.abs(0x8D, 0x0200);
    a.abs(0x8D, 0x0203);
    a.emit({0xA9, 0x01});
    a.abs(0x8D, 0x0201);
    a.abs(0x8D, 0x4015);  // the first pulse on
    a.emit({0xA9, 0x00});
    a.abs(0x8D, 0x0202);
    a.abs(0x8D, 0x2005);
    a.abs(0x8D, 0x2005);
    a.emit({0xA9, 0x80});
    a.abs(0x8D, 0x2000);  // NMI on
    a.emit({0xA9, 0x1E});
    a.abs(0x8D, 0x2001);  // background and sprites, the left column too
    loop = a.code.size();
    a.abs(0x4C, static_cast<uint16_t>(0x8000 + loop));

    nmi = a.code.size();
    a.emit({0xA9, 0x02});
    a.abs(0x8D, 0x4014);  // sprite DMA from 0200
    a.emit({0xA9, 0x01});
    a.abs(0x8D, 0x4016);
    a.emit({0xA9, 0x00});
    a.abs(0x8D, 0x4016);
    a.abs(0xAD, 0x4016);  // A
    a.emit({0x29, 0x01});
    a.branch(0xF0, &noA);
    a.emit({0xA9, 0xBF});
    a.abs(0x8D, 0x4000);
    a.emit({0xA9, 0xFD});
    a.abs(0x8D, 0x4002);
    a.emit({0xA9, 0x08});
    a.abs(0x8D, 0x4003);
    noA = a.code.size();
    for (int i = 0; i < 5; ++i) a.abs(0xAD, 0x4016);  // B Select Start Up Down
    a.abs(0xAD, 0x4016);  // Left
    a.emit({0x29, 0x01});
    a.branch(0xF0, &noL);
    a.abs(0xCE, 0x0203);
    noL = a.code.size();
    a.abs(0xAD, 0x4016);  // Right
    a.emit({0x29, 0x01});
    a.branch(0xF0, &noR);
    a.abs(0xEE, 0x0203);
    noR = a.code.size();
    a.emit({0xE6, 0x10, 0xA5, 0x10});  // inc $10 lda $10
    a.abs(0x8D, 0x2005);
    a.emit({0xA9, 0x00});
    a.abs(0x8D, 0x2005);
    a.emit({0x40});  // rti
    a.resolve();

    std::vector<uint8_t> rom = {'N', 'E', 'S', 0x1A, 1, 1, static_cast<uint8_t>(battery ? 0x02 : 0x00), 0};
    rom.resize(16 + 0x4000 + 0x2000, 0);
    std::copy(a.code.begin(), a.code.end(), rom.begin() + 16);
    auto vector = [&](size_t at, uint16_t to) {
        rom[16 + at] = static_cast<uint8_t>(to);
        rom[16 + at + 1] = static_cast<uint8_t>(to >> 8);
    };
    vector(0x3FFA, static_cast<uint16_t>(0x8000 + nmi));
    vector(0x3FFC, 0x8000);
    vector(0x3FFE, static_cast<uint16_t>(0x8000 + loop));
    for (int i = 0; i < 8; ++i) rom[16 + 0x4000 + 16 + i] = 0xFF;  // tile 1: solid colour 1
    return rom;
}

// A small SNES program (LoROM, 32 KB), so that the SNES can be tested without any
// game: mode 0 with vertical stripes on BG1 that scroll one pixel a frame, and a red
// sprite at (0x50, 0x50) that the cross moves. Its work is done in the NMI, with the
// pads read by the automatic read.
std::vector<uint8_t> snesProgram(bool battery = false, uint8_t region = 0x01) {
    struct Asm {
        std::vector<uint8_t> code;
        void emit(std::initializer_list<int> bytes) {
            for (int b : bytes) code.push_back(static_cast<uint8_t>(b));
        }
        void abs(int op, uint16_t a) { emit({op, a & 0xFF, a >> 8}); }
        size_t here() const { return code.size(); }
        void branch(int op, size_t to) { emit({op, static_cast<int>(to) - static_cast<int>(code.size() + 2)}); }
    } a;
    a.emit({0x78, 0x18, 0xFB, 0xC2, 0x10, 0xA2, 0xFF, 0x1F, 0x9A, 0xE2, 0x30});  // sei clc xce rep #$10 ldx #$1FFF txs sep #$30
    a.emit({0xA9, 0x80});
    a.abs(0x8D, 0x2100);  // forced blank
    // Palette: black backdrop, white colour 1, red for the sprites' colour 1.
    a.abs(0x9C, 0x2121);
    a.abs(0x9C, 0x2122);
    a.abs(0x9C, 0x2122);
    a.emit({0xA9, 0xFF});
    a.abs(0x8D, 0x2122);
    a.emit({0xA9, 0x7F});
    a.abs(0x8D, 0x2122);
    a.emit({0xA9, 0x81});
    a.abs(0x8D, 0x2121);
    a.emit({0xA9, 0x1F});
    a.abs(0x8D, 0x2122);
    a.abs(0x9C, 0x2122);
    // VRAM: tile 1 solid in 2bpp (words 8-15) and in 4bpp (words 16-23), the map at 0400.
    a.emit({0xA9, 0x80});
    a.abs(0x8D, 0x2115);
    a.emit({0xA9, 0x08});
    a.abs(0x8D, 0x2116);
    a.abs(0x9C, 0x2117);
    a.emit({0xA2, 0x10});  // ldx #16: both tiles, one after the other
    size_t l = a.here();
    a.emit({0xA9, 0xFF});
    a.abs(0x8D, 0x2118);
    a.abs(0x9C, 0x2119);
    a.emit({0xCA});
    a.branch(0xD0, l);
    a.abs(0x9C, 0x2116);
    a.emit({0xA9, 0x04});
    a.abs(0x8D, 0x2117);
    a.emit({0xC2, 0x10, 0xA2, 0x00, 0x04});  // rep #$10 ldx #$0400
    l = a.here();
    a.emit({0x8A, 0x29, 0x01});  // txa and #1
    a.abs(0x8D, 0x2118);
    a.abs(0x9C, 0x2119);
    a.emit({0xCA});
    a.branch(0xD0, l);
    a.emit({0xE2, 0x30});
    a.emit({0xA9, 0x04});
    a.abs(0x8D, 0x2107);  // BG1's map at 0400
    a.abs(0x9C, 0x2105);  // mode 0
    a.abs(0x9C, 0x210B);
    a.abs(0x9C, 0x2101);
    // OAM: the sprite, and the other 127 below the picture.
    a.abs(0x9C, 0x2102);
    a.abs(0x9C, 0x2103);
    for (int v : {0x50, 0x50, 0x01, 0x30}) {
        a.emit({0xA9, v});
        a.abs(0x8D, 0x2104);
    }
    a.emit({0xA2, 0x7F});
    l = a.here();
    a.abs(0x9C, 0x2104);
    a.emit({0xA9, 0xE0});
    a.abs(0x8D, 0x2104);
    a.abs(0x9C, 0x2104);
    a.abs(0x9C, 0x2104);
    a.emit({0xCA});
    a.branch(0xD0, l);
    a.emit({0xA9, 0x50, 0x85, 0x10, 0x64, 0x11});  // x = $50, scroll = 0
    a.emit({0xA9, 0x11});
    a.abs(0x8D, 0x212C);  // BG1 and sprites on the main screen
    a.emit({0xA9, 0x81});
    a.abs(0x8D, 0x4200);  // NMI and the automatic pad read
    a.emit({0xA9, 0x0F});
    a.abs(0x8D, 0x2100);
    const size_t loop = a.here();
    a.branch(0x80, loop);

    const size_t nmi = a.here();
    l = a.here();
    a.abs(0xAD, 0x4212);
    a.emit({0x29, 0x01});
    a.branch(0xD0, l);  // until the pads are read
    a.abs(0xAD, 0x4219);
    a.emit({0x29, 0x01});
    a.emit({0xF0, 0x02, 0xE6, 0x10});  // beq +2, inc $10: right
    a.abs(0xAD, 0x4219);
    a.emit({0x29, 0x02});
    a.emit({0xF0, 0x02, 0xC6, 0x10});  // left
    a.abs(0x9C, 0x2102);
    a.abs(0x9C, 0x2103);
    a.emit({0xA5, 0x10});
    a.abs(0x8D, 0x2104);
    for (int v : {0x50, 0x01, 0x30}) {
        a.emit({0xA9, v});
        a.abs(0x8D, 0x2104);
    }
    a.emit({0xE6, 0x11, 0xA5, 0x11});  // inc $11 lda $11
    a.abs(0x8D, 0x210D);
    a.abs(0x9C, 0x210D);
    a.abs(0xAD, 0x4210);
    a.emit({0x40});

    std::vector<uint8_t> rom(0x8000, 0);
    std::copy(a.code.begin(), a.code.end(), rom.begin());
    const char* title = "PLUMNIN TEST";
    std::fill(rom.begin() + 0x7FC0, rom.begin() + 0x7FD5, ' ');
    std::copy(title, title + std::strlen(title), rom.begin() + 0x7FC0);
    rom[0x7FD5] = 0x20;
    rom[0x7FD6] = battery ? 0x02 : 0x00;
    rom[0x7FD7] = 0x05;
    rom[0x7FD8] = battery ? 0x01 : 0x00;
    rom[0x7FD9] = region;
    rom[0x7FDC] = rom[0x7FDD] = 0xFF;  // complement FFFF, checksum 0000
    auto vector = [&](size_t v, size_t to) {
        rom[v] = static_cast<uint8_t>(0x8000 + to);
        rom[v + 1] = static_cast<uint8_t>((0x8000 + to) >> 8);
    };
    vector(0x7FEA, nmi);
    vector(0x7FFC, 0);
    return rom;
}

void testSystems() {
    namespace fs = std::filesystem;
    std::vector<uint8_t> nes = {'N', 'E', 'S', 0x1A, 2, 1};
    nes.resize(16 + 0x8000 + 0x2000);
    check(detectSystemFromData(nes) == ConsoleSystem::Nes, "systems: an iNES header");
    check(detectSystemFromData(gameBoyRom(false)) == ConsoleSystem::GameBoy, "systems: the Game Boy's logo");
    check(detectSystemFromData(gameBoyRom(true)) == ConsoleSystem::GameBoyColor, "systems: a Color-only cartridge");
    check(detectSystemFromData(std::vector<uint8_t>(0x8000, 0xEA)) == ConsoleSystem::Snes, "systems: anything else is the SNES's");
    check(isGameFile("a.gb") && isGameFile("a.GBC") && isGameFile("a.nes") && isGameFile("a.sfc") && isGameFile("a.smc") &&
              isGameFile("a.zip") && !isGameFile("notes.txt") && !isGameFile("a.md"),
          "systems: game file extensions");
    check(std::string(systemName(ConsoleSystem::Snes)) == "Super Nintendo" && std::string(systemKey(ConsoleSystem::Nes)) == "NES",
          "systems: names");

    const fs::path dir = fs::temp_directory_path() / "plumnin_systems_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeFile(dir / "Tetris (World).gb", gameBoyRom(false));
    writeFile(dir / "Color (USA).gb", gameBoyRom(true));
    writeFile(dir / "Zelda (USA).nes", nes);
    writeFile(dir / "Mario (USA).sfc", std::vector<uint8_t>(0x8000, 0));
    writeZip((dir / "Zipped.zip").string(), {{"README.txt", {'h', 'i'}}, {"games/Color (USA).gbc", gameBoyRom(true)}});
    writeZip((dir / "Inside.zip").string(), {{"Inner.gb", gameBoyRom(false)}});
    check(detectSystem((dir / "Tetris (World).gb").string()) == ConsoleSystem::GameBoy &&
              detectSystem((dir / "Color (USA).gb").string()) == ConsoleSystem::GameBoyColor &&
              detectSystem((dir / "Zelda (USA).nes").string()) == ConsoleSystem::Nes &&
              detectSystem((dir / "Mario (USA).sfc").string()) == ConsoleSystem::Snes &&
              detectSystem((dir / "Zipped.zip").string()) == ConsoleSystem::GameBoyColor &&
              detectSystem((dir / "Inside.zip").string()) == ConsoleSystem::GameBoy,
          "systems: detected from the files, zipped ones too");
    std::vector<uint8_t> rom;
    std::string name, error;
    check(readRomFile((dir / "Zipped.zip").string(), rom, &name, &error) && rom == gameBoyRom(true) &&
              name == "games/Color (USA).gbc",
          "systems: the cartridge inside a zip, not its other files");
    writeZip((dir / "Empty.zip").string(), {{"README.txt", {'h', 'i'}}});
    check(!readRomFile((dir / "Empty.zip").string(), rom, &name, &error) && !error.empty(), "systems: a zip without a cartridge");
    writeFile(dir / "Broken.zip", {'P', 'K', 3, 4, 0, 0});
    check(!readRomFile((dir / "Broken.zip").string(), rom, &name, &error) && error.find("dañado") != std::string::npos,
          "systems: a damaged zip");
    fs::create_directories(dir / "Folder.nes");
    check(!readRomFile((dir / "Folder.nes").string(), rom, &name, &error) && !error.empty(),
          "systems: a folder named like a game is refused");
    fs::remove_all(dir);
}

// Until each console is emulated, opening its games says so (and nothing breaks).
void testConsoles() {
    for (int s = 0; s < kSystemCount; ++s) {
        const ConsoleSystem system = static_cast<ConsoleSystem>(s);
        if (emulated(system)) continue;
        check(!createConsole(system), "consoles: no console for one not emulated yet");
    }
    const std::string path = (std::filesystem::temp_directory_path() / "plumnin_consoles_test.nes").string();
    writeFile(path, {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    std::string error;
    if (!emulated(ConsoleSystem::Nes))
        check(!openGame(path, &error) && error.find("NES") != std::string::npos, "consoles: a game of one not emulated yet");
    std::remove(path.c_str());
}

void testTestConsole() {
    auto c = testConsole();
    const int x0 = c->spriteX(), y0 = c->spriteY();
    check(x0 == 120 && y0 == 104 && c->frame().width == 256 && c->frame().height == 224,
          "test pattern: a TV's picture with the sprite in the middle");
    PadState right;
    right.right = right.down = true;
    for (int i = 0; i < 10; ++i) {
        c->setPads(right, PadState{});
        c->runFrame();
    }
    check(c->spriteX() == x0 + 10 && c->spriteY() == y0 + 10 && c->frames() == 10, "test pattern: the first pad moves the sprite");
    PadState left;
    left.left = true;
    c->setPads(PadState{}, left);
    c->runFrame();
    check(c->spriteX() == x0 + 9, "test pattern: so does the second");
    PadState a;
    a.a = true;
    c->audio().clearSamples();
    c->setPads(a, PadState{});
    c->runFrame();
    const auto& s = c->audio().samples();
    check(s.size() == 2 * 735 && std::any_of(s.begin(), s.end(), [](int16_t v) { return v != 0; }),
          "test pattern: a frame of stereo sound, a tone while A is held");
    const VideoFrame& f = c->frame();
    const size_t at = static_cast<size_t>(c->spriteY() + 5) * VideoFrame::kMaxWidth + static_cast<size_t>(c->spriteX() + 5);
    check(f.info[at].layer == Layer::Sprite && f.rgba[at] == f.sprites[at], "test pattern: the sprite on top, as the layers say");
    c->setWidescreen(43, 43);
    c->runFrame();
    check(c->frame().width == 256 + 86 && c->frame().consoleWidth() == 256, "test pattern: widescreen columns");

    auto gb = testConsole(ConsoleSystem::GameBoy);
    gb->setWidescreen(43, 43);
    gb->runFrame();
    check(gb->frame().width == 160 && gb->frame().height == 144 && gb->frame().screenAspect > 1.1f && gb->ports() == 1,
          "test pattern: the Game Boy's screen, without widescreen");
    check(wideColumnsFor(16, 9) == 43 && wideColumnsFor(16, 10) == 26 && wideColumnsFor(4, 3) == 0 &&
              wideColumnsFor(32, 9) == VideoFrame::kMaxWideSide,
          "video: widescreen columns for each shape");
}

void testStates() {
    auto c = testConsole();
    for (int i = 0; i < 30; ++i) c->runFrame();
    c->audio().clearSamples();
    std::vector<uint8_t> state;
    check(c->saveState(state) && state.size() > 100, "savestate: saved");

    auto run = [&](int frames, std::vector<uint32_t>& hashes, std::vector<int16_t>& sound) {
        PadState p;
        for (int i = 0; i < frames; ++i) {
            p.right = (i / 7) % 2 == 0;  // some input, the same both times
            p.a = (i / 5) % 3 == 0;
            c->setPads(p, PadState{});
            c->runFrame();
            hashes.push_back(frameHash(c->frame()));
            sound.insert(sound.end(), c->audio().samples().begin(), c->audio().samples().end());
            c->audio().clearSamples();
        }
    };
    std::vector<uint32_t> h1, h2;
    std::vector<int16_t> s1, s2;
    run(90, h1, s1);
    std::string error;
    check(c->loadState(state, &error), "savestate: loaded");
    run(90, h2, s2);
    check(h1 == h2 && std::set<uint32_t>(h1.begin(), h1.end()).size() > 10, "savestate: the same frames after loading");
    check(soundHash(s1) == soundHash(s2) && s1.size() == s2.size(), "savestate: the same sound after loading");

    std::vector<uint8_t> again;
    c->loadState(state, &error);
    c->saveState(again);
    check(again == state, "savestate: save, load, save gives the same bytes");

    std::vector<uint32_t> pixels;
    int tw = 0, th = 0;
    check(Console::stateThumbnail(state, pixels, &tw, &th) && tw == 128 && th == 112 && pixels.size() == 128u * 112u,
          "savestate: thumbnail at half size");

    // Damaged files are rejected and leave the console as it was.
    c->loadState(state, &error);
    std::vector<uint8_t> before;
    c->saveState(before);
    bool allRejected = true;
    std::vector<uint8_t> bad(state.begin(), state.begin() + static_cast<long>(state.size() / 2));
    allRejected &= !c->loadState(bad, &error);
    bad = {'P', 'L', 'N', 'S', 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
    allRejected &= !c->loadState(bad, &error);
    std::mt19937 rng(1234);
    int loaded = 0;
    for (int i = 0; i < 200; ++i) {
        bad = state;
        for (int k = 0; k < 4; ++k) bad[12 + rng() % (bad.size() - 12)] ^= static_cast<uint8_t>(1 + rng() % 255);
        if (c->loadState(bad, &error)) {
            ++loaded;
            c->loadState(state, &error);
        }
    }
    std::vector<uint8_t> after;
    c->saveState(after);
    check(allRejected && after == before, "savestate: truncated or random files are rejected safely");
    std::printf("     (%d of 200 randomly damaged states still decompressed and loaded)\n", loaded);

    auto other = testConsole(ConsoleSystem::Nes, 2);  // another cartridge, another CRC
    check(!other->loadState(state, &error) && error.find("otra ROM") != std::string::npos,
          "savestate: a state from another ROM is rejected");
    auto snes = testConsole(ConsoleSystem::Snes);
    check(!snes->loadState(state, &error) && error.find("otra consola") != std::string::npos,
          "savestate: a state from another console is rejected");
}

void testRewindPlayback() {
    // Rewinding n frames and playing them again with the same pads gives the same
    // picture as the first time: the snapshots are full states.
    auto c = testConsole();
    RewindBuffer r;
    r.setLimits(600, size_t{64} << 20);
    std::vector<uint32_t> first;
    auto pads = [](int i) {
        PadState p;
        p.right = (i / 5) % 2 == 0;
        p.a = i % 3 == 0;
        return p;
    };
    for (int i = 0; i < 120; ++i) {
        std::vector<uint8_t> s;
        c->saveState(s, true);
        r.push(std::move(s));
        c->setPads(pads(i), PadState{});
        c->runFrame();
        c->audio().clearSamples();
        if (i >= 60) first.push_back(frameHash(c->frame()));
    }
    std::vector<uint8_t> s;
    std::string error;
    bool ok = true;
    for (int i = 0; i < 60; ++i) ok &= r.pop(s);
    ok &= c->loadState(s, &error);
    std::vector<uint32_t> again;
    for (int i = 60; i < 120; ++i) {
        c->setPads(pads(i), PadState{});
        c->runFrame();
        c->audio().clearSamples();
        again.push_back(frameHash(c->frame()));
    }
    check(ok && r.size() == 60, "rewind: went back 60 frames");
    check(again == first, "rewind: replaying from there gives the same frames");

    std::vector<uint8_t> quick;
    c->saveState(quick, true);
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    check(!Console::stateThumbnail(quick, pixels, &w, &h), "rewind: quick snapshots carry no thumbnail");
}

void testRunAhead() {
    // Run-ahead shows the next frame now and leaves no trace: the console goes on as if
    // it had never run ahead, with the same sound.
    auto pads = [](int i) {
        PadState p;
        p.right = (i / 10) % 2 == 0;
        p.a = (i / 7) % 2 == 0;
        return p;
    };
    auto plain = testConsole(), ahead = testConsole();
    std::vector<uint32_t> plainFrames, aheadFrames;
    std::vector<int16_t> plainSound, aheadSound;
    bool back = true;
    for (int i = 0; i < 120; ++i) {
        plain->setPads(pads(i), PadState{});
        plain->runFrame();
        plainFrames.push_back(frameHash(plain->frame()));
        plainSound.insert(plainSound.end(), plain->audio().samples().begin(), plain->audio().samples().end());
        plain->audio().clearSamples();

        ahead->setPads(pads(i), PadState{});
        ahead->setDrawing(false);
        ahead->runFrame();
        ahead->setDrawing(true);
        aheadSound.insert(aheadSound.end(), ahead->audio().samples().begin(), ahead->audio().samples().end());
        ahead->audio().clearSamples();
        back &= ahead->runAhead(1);
        aheadFrames.push_back(frameHash(ahead->frame()));
    }
    // Frame i shown with run-ahead is frame i + 1, wherever the pads did not change.
    bool early = true;
    int compared = 0;
    for (int i = 0; i + 1 < 120; ++i) {
        if (pads(i).right != pads(i + 1).right || pads(i).a != pads(i + 1).a) continue;
        early &= aheadFrames[static_cast<size_t>(i)] == plainFrames[static_cast<size_t>(i + 1)];
        ++compared;
    }
    std::vector<uint8_t> a, b;
    plain->saveState(a, true);
    ahead->saveState(b, true);
    check(back, "run-ahead: the console went back every frame");
    const std::set<uint32_t> pictures(plainFrames.begin(), plainFrames.end());
    check(early && compared > 90 && pictures.size() > 1, "run-ahead: the picture is the next frame's");
    check(a == b && plain->frames() == ahead->frames(), "run-ahead: the console is where it would have been");
    check(aheadSound == plainSound, "run-ahead: the sound is the same");
}

void testPlanes() {
    auto c = testConsole();
    for (int i = 0; i < 20; ++i) c->runFrame();
    const VideoFrame& f = c->frame();
    for (const bool highB : {false, true}) {
        Planes planes;
        splitPlanes(f, SplitOptions{highB, false}, planes);
        // Golden rule: looking straight at the planes, front to back, shows the 2D picture.
        bool same = planes.width == f.width && planes.height == f.height;
        bool skyFull = true;
        for (int y = 0; y < planes.height && same; ++y)
            for (int x = 0; x < planes.width; ++x) {
                const size_t o = static_cast<size_t>(y) * planes.width + x;
                uint32_t seen = 0;
                for (int p = kPlanes - 1; p >= 0 && !seen; --p)
                    if (planes[p][o] >> 24) seen = planes[p][o];
                if ((seen & 0xFFFFFF) != (f.pixel(x, y) & 0xFFFFFF)) same = false;
                skyFull = skyFull && (planes[PlaneSky][o] >> 24);
            }
        check(same, highB ? "planes: straight on = 2D (high B in front)" : "planes: straight on = 2D");
        check(skyFull, "planes: the sky has no holes");
    }
    Planes planes;
    splitPlanes(f, SplitOptions{}, planes);
    const size_t sprite = static_cast<size_t>(c->spriteY() + 4) * static_cast<size_t>(planes.width) +
                          static_cast<size_t>(c->spriteX() + 4);
    check(planes[PlaneSprites][sprite] != 0, "planes: the sprite on its own plane");
}

void testEdgeDistance() {
    constexpr int kW = 64, kH = 48;
    std::vector<uint32_t> plane(kW * kH, 0);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x) plane[y * kW + x] = 0xFF123456u;
    markEdgeDistance(plane, kW, kH);
    auto distance = [&](int x, int y) { return kMaxEdgeDistance - (255 - int(plane[y * kW + x] >> 24)) / 16; };
    check(distance(10, 10) == 1 && distance(29, 20) == 1, "edge distance: 1 on the outline");
    check(distance(11, 20) == 2 && distance(20, 12) == 3, "edge distance: grows inwards");
    check(distance(20, 20) == kMaxEdgeDistance, "edge distance: capped in the middle");
    check(plane[0] == 0 && (plane[20 * kW + 20] & 0xFFFFFF) == 0x123456, "edge distance: colours untouched");
}

void testLiveCamera() {
    auto frame = std::make_unique<VideoFrame>();
    Planes planes;
    planes.width = 256;
    planes.height = 224;
    for (auto& p : planes.layer) p.assign(256 * 224, 0);
    // A sprite on the right of the screen, nothing scrolling.
    for (int y = 100; y < 120; ++y)
        for (int x = 220; x < 240; ++x) planes[PlaneSprites][static_cast<size_t>(y) * 256 + x] = 0xFFFFFFFFu;
    LiveCamera cam;
    for (int i = 0; i < 300; ++i) {
        frame->frameNumber = static_cast<uint64_t>(i + 1);
        cam.update(*frame, planes);
    }
    check(cam.yaw() > 1.0f && cam.yaw() <= LiveCamera::kSwayYaw && std::fabs(cam.tilt()) < 1.0f,
          "live camera: leans towards the sprites, within its limit");

    // No sprites, the level scrolling left (the player running right), wrapping round.
    for (auto& p : planes.layer) p.assign(256 * 224, 0);
    LiveCamera run;
    int scroll = 5;
    for (int i = 0; i < 300; ++i) {
        frame->frameNumber = static_cast<uint64_t>(i + 1);
        for (auto& l : frame->lines) l.hscrollA = static_cast<int16_t>(scroll);
        scroll = (scroll - 4 + 1024) % 1024;
        run.update(*frame, planes);
    }
    check(run.yaw() > 1.0f && run.yaw() <= LiveCamera::kSwayYaw, "live camera: leans with the scroll");
    const float before = run.yaw();
    for (int i = 0; i < 10; ++i) run.update(*frame, planes);  // the same frame again: only eases
    check(std::fabs(run.yaw() - before) < 0.5f, "live camera: a repeated frame does not jolt it");
}

void testCheats() {
    // A made-up code kind, "AAAA:VV": a byte written to RAM, as each console's decoder will do.
    const CheatDecoder decoder = [](const std::string& code, CheatPatch* out, std::string* error) {
        unsigned address = 0, value = 0;
        char tail = 0;
        if (std::sscanf(code.c_str(), "%4x:%2x%c", &address, &value, &tail) != 2) {
            if (error) *error = "no";
            return false;
        }
        *out = {address, static_cast<uint16_t>(value), true, -1, true};
        return true;
    };
    std::vector<CheatPatch> many;
    std::string error;
    check(decodeCheatCodes("00C0:12 + 00c1:34", decoder, &many, &error) && many.size() == 2 && many[0].address == 0xC0 &&
              many[1].value == 0x34,
          "cheats: codes joined with + give all their writes");
    check(!decodeCheatCodes("00C0:12+XX", decoder, &many, &error) && many.empty(), "cheats: one bad part refuses the whole code");
    check(!decodeCheatCodes("+", decoder, &many, &error), "cheats: nothing between the + is no code");
    check(!decodeCheatCodes("00C0:12", CheatDecoder{}, &many, &error) && !error.empty(),
          "cheats: a console without cheats yet says so");
    check(!cheatDecoderFor(ConsoleSystem::Nes) || cheatFormatsFor(ConsoleSystem::Nes)[0], "cheats: a console's codes have a name");

    std::vector<Cheat> cheats = {{"ABCD-EFGH", "Vidas infinitas", true}, {"00C0:12", "", false}};
    const std::vector<Cheat> back = parseCheats(formatCheats(cheats));
    check(back.size() == 2 && back[0].code == "ABCD-EFGH" && back[0].name == "Vidas infinitas" && back[0].enabled &&
              back[1].code == "00C0:12" && back[1].name.empty() && !back[1].enabled,
          "cheats: cheat file round trip");
    const std::string cht = "cheats = 3\n\ncheat0_desc = \"Infinite lives\"\ncheat0_code = \"SXIOPO\"\n"
                            "cheat0_enable = false\n\ncheat1_desc = \"Rumble\"\ncheat1_code = \"\"\ncheat1_handler = 1\n"
                            "cheat2_desc = \"Infinite Energy\"\r\ncheat2_code = \"aeuzz+01FF0763\"\r\ncheat2_enable = true\r\n";
    const std::vector<Cheat> fromCht = parseChtFile(cht);
    check(fromCht.size() == 2 && fromCht[0].name == "Infinite lives" && fromCht[0].code == "SXIOPO" && !fromCht[0].enabled &&
              fromCht[1].code == "AEUZZ+01FF0763" && !fromCht[1].enabled,
          "cheats: RetroArch .cht lists, without the entries that have no code");
    const std::vector<Cheat> kept = parseCheats(formatCheats(fromCht));
    check(kept.size() == 2 && kept[1].code == fromCht[1].code && kept[1].name == "Infinite Energy",
          "cheats: a joined code survives the cheat file");
}

void testRewindBuffer() {
    RewindBuffer r;
    r.setLimits(3, 1000);
    for (uint8_t i = 1; i <= 5; ++i) r.push(std::vector<uint8_t>(10, i));
    std::vector<uint8_t> s;
    check(r.size() == 3 && r.bytes() == 30, "rewind: keeps only the newest snapshots");
    check(r.pop(s) && s[0] == 5 && r.pop(s) && s[0] == 4 && r.pop(s) && s[0] == 3 && !r.pop(s) && r.bytes() == 0,
          "rewind: pops newest first, then runs out");
    r.setLimits(100, 25);
    for (uint8_t i = 1; i <= 5; ++i) r.push(std::vector<uint8_t>(10, i));
    check(r.size() == 2 && r.bytes() == 20, "rewind: the byte limit drops the oldest too");
    r.setLimits(0, 1000);
    r.push(std::vector<uint8_t>(10, 9));
    check(r.empty(), "rewind: no frames means off");
}

void testFramePacer() {
    auto total = [](double speed, int ticks, int* most = nullptr) {
        FramePacer p;
        int sum = 0, top = 0;
        for (int i = 0; i < ticks; ++i) {
            const int n = p.framesForTick(speed);
            sum += n;
            top = std::max(top, n);
        }
        if (most) *most = top;
        return sum;
    };
    int most = 0;
    check(total(1.0, 60, &most) == 60 && most == 1, "speed: normal runs one frame per tick");
    check(total(4.0, 60, &most) == 240 && most == 4, "speed: fast forward x4");
    check(total(0.5, 60, &most) == 30 && most == 1, "speed: slow motion at 50% runs every other tick");
    check(total(0.1, 100) == 10, "speed: 10% adds up without losing frames to rounding");
    check(total(2.5, 4) == 10, "speed: fractions carry over between ticks");
}

void testProfiles() {
    RenderOptions r;
    r.pseudo3d = true;
    r.style = Style3D::Layers;
    r.depth = 0.8f;
    r.tilt = 40;
    r.yaw = -30;
    r.zoom = 1.5f;
    r.extrusion = 0.2f;
    r.thickness = 0.7f;
    r.shadows = false;
    r.highBInFront = true;
    r.flatB = true;
    r.tiltCamera = false;
    r.lighting = false;
    r.liveCamera = true;
    r.smooth = true;  // not part of a profile
    const std::string text = formatProfile(r, "Sonic.md");
    RenderOptions back;
    check(parseProfile(text, back) && same3D(back, r), "profile: written and read back the same");
    check(!back.smooth, "profile: only carries the 3D options");

    RenderOptions partial;
    const RenderOptions defaults;
    check(parseProfile("[profile_3d]\ntilt_3d = 500\ndepth_3d = 30\n", partial) && partial.tilt == kMaxTilt &&
              std::fabs(partial.depth - 0.3f) < 1e-6f && partial.yaw == defaults.yaw,
          "profile: missing keys keep their value, out of range ones are clamped");
    RenderOptions untouched;
    check(!parseProfile("[video]\ntilt_3d = 10\n", untouched) && untouched.tilt == defaults.tilt,
          "profile: a file without [profile_3d] is not a profile");

    RenderOptions target;
    target.smooth = true;
    copy3D(r, target);
    check(same3D(target, r) && target.smooth, "profile: copy3D leaves the other options alone");
}

// The detail panel's data: libretro-database's metadat files and Wikipedia's answers.
void testGameInfo() {
    const std::string genres = "clrmamepro (\n\tname \"Sega - Mega Drive - Genesis\"\n)\n\n"
                               "game (\n\tcomment \"Sonic The Hedgehog 2 (World)\"\n\tgenre \"Platform\"\n"
                               "\trom ( crc 24AB4C3A )\n)\n\n"
                               "game (\n\tcomment \"Sonic The Hedgehog 2 (World) (Rev A)\"\n\tgenre \"Platform\"\n)\n\n"
                               "game (\n\tcomment \"Columns (World)\"\n\trom ( crc 03163D7A )\n)\n";
    const auto entries = parseMetadat(genres, "genre");
    check(entries.size() == 2 && entries[0].first == "Sonic The Hedgehog 2 (World)" && entries[0].second == "Platform",
          "game info: metadat entries with the field, the others left out");
    const std::string users = "game (\n\tcomment \"Streets of Rage 2 (USA)\"\n\tusers 2\n\trom ( crc E01FA526 )\n)\n";
    check(parseMetadat(users, "users").size() == 1 && parseMetadat(users, "users")[0].second == "2",
          "game info: unquoted values");
    DetailsTable table;
    table.add("genre", genres);
    table.add("maxusers", users);
    table.add("developer", "game (\n\tcomment \"Sonic The Hedgehog 2 (World)\"\n\tdeveloper \"SEGA\"\n)\n");
    const GameDetails* d = table.find("Sonic The Hedgehog 2 (World)");
    check(d && d->genre == "Platform" && d->developer == "SEGA", "game info: the fields of one game together");
    d = table.find("Sonic The Hedgehog 2 (Europe) [!]");
    check(d && d->developer == "SEGA", "game info: another region of the same title");
    d = table.find("streets of rage 2 (usa)");
    check(d && d->players == "2", "game info: names without case");
    check(!table.find("Ecco the Dolphin (Europe)"), "game info: a game it does not know");

    const std::string answer =
        "{\"batchcomplete\":true,\"query\":{\"pages\":[{\"pageid\":2,\"ns\":0,\"title\":\"Sonic the Hedgehog\","
        "\"index\":2,\"extract\":\"Otro.\"},{\"pageid\":1,\"ns\":0,\"title\":\"Sonic the Hedgehog 2\",\"index\":1,"
        "\"extract\":\"Sonic the Hedgehog 2 es un videojuego de plataformas \\u00e9\\n\\\"Sega\\\".\"}]}}";
    const auto pages = parseWikiSearch(answer);
    check(pages.size() == 2 && pages[0].title == "Sonic the Hedgehog 2" && pages[0].index == 1,
          "game info: Wikipedia's pages in search order");
    check(!pages.empty() && pages[0].extract == "Sonic the Hedgehog 2 es un videojuego de plataformas \xc3\xa9\n\"Sega\".",
          "game info: Wikipedia's escapes");
    check(wikiTitleDistance("Sonic The Hedgehog 2", "Sonic the Hedgehog 2") == 0, "game info: the same title");
    check(wikiTitleDistance("Sonic The Hedgehog 2", "Sonic the Hedgehog") < 0, "game info: not the first game for the second");
    check(wikiTitleDistance("Golden Axe II", "Golden Axe 2 (videojuego)") == 0, "game info: roman numerals and notes");
    check(wikiTitleDistance("Phantasy Star IV", "Phantasy Star IV: The End of the Millennium") > 0,
          "game info: a longer title of the same game");
    check(wikiTitleDistance("Sonic 3D Blast", "Sonic") < 0, "game info: too little in common");
    check(wikiTitleDistance("Columns", "Ecco the Dolphin") < 0, "game info: nothing in common");
}

void testScreenScraper() {
    // A jeuInfos answer as the API gives it (trimmed), with its trailing comma.
    const std::string info =
        "{\"header\":{\"success\":\"true\"},\"response\":{\"ssuser\":{\"requeststoday\":\"12\",\"maxrequestsperday\":\"20000\"},"
        "\"jeu\":{\"id\":\"3\",\"systeme\":{\"id\":\"1\",\"text\":\"Megadrive\"},"
        "\"noms\":[{\"region\":\"wor\",\"text\":\"Sonic The Hedgehog\"},{\"region\":\"jp\",\"text\":\"\\u30bd\\u30cb\\u30c3\\u30af\"}],"
        "\"editeur\":{\"id\":\"1\",\"text\":\"SEGA\"},\"developpeur\":{\"text\":\"Sonic Team\"},\"joueurs\":{\"text\":\"1\"},"
        "\"note\":{\"text\":\"17\"},\"dates\":[{\"region\":\"us\",\"text\":\"1991-06-23\"},{\"region\":\"eu\",\"text\":\"1991-06-21\"}],"
        "\"synopsis\":[{\"langue\":\"en\",\"text\":\"Run.\"},{\"langue\":\"es\",\"text\":\"Corre.\"}],"
        "\"genres\":[{\"id\":\"7\",\"noms\":[{\"langue\":\"en\",\"text\":\"Platform\"},{\"langue\":\"es\",\"text\":\"Plataformas\"}]}],"
        "\"medias\":[{\"type\":\"box-2D\",\"region\":\"us\",\"format\":\"png\","
        "\"url\":\"https://neoclone.screenscraper.fr/api2/mediaJeu.php?devid=d&devpassword=p&softname=s&ssid=u&sspassword=x&systemeid=1&jeuid=3&media=box-2D(us)\"},"
        "{\"type\":\"box-2D\",\"region\":\"eu\",\"format\":\"png\",\"url\":\"https://a.screenscraper.fr/api2/mediaJeu.php?jeuid=3&media=box-2D(eu)\"},"
        "{\"type\":\"wheel\",\"region\":\"wor\",\"format\":\"png\",\"url\":\"https://a.screenscraper.fr/api2/mediaJeu.php?media=wheel(wor)\"},"
        "{\"type\":\"fanart\",\"format\":\"jpg\",\"url\":\"https://a.screenscraper.fr/api2/mediaJeu.php?media=fanart\"},"
        "]}}}";
    SsAnswer a = parseSsAnswer(200, info);
    check(a.result == SsAnswer::Ok && a.games.size() == 1, "screenscraper: one game");
    if (a.games.size() == 1) {
        const SsGame& g = a.games[0];
        const auto regions = ssRegions("Europe · Rev A");
        check(g.id == 3 && g.system == 1 && g.publisher == "SEGA" && g.rating == "17", "screenscraper: the game's data");
        check(g.name(regions) == "Sonic The Hedgehog" && g.date(regions) == "1991-06-21" && g.year(regions) == "1991",
              "screenscraper: names and dates in the ROM's region");
        check(g.text(ssLanguages("es")) == "Corre." && g.genre(ssLanguages("es")) == "Plataformas",
              "screenscraper: texts in the user's language");
        check(g.names.size() == 2 && g.names[1].second == "\xe3\x82\xbd\xe3\x83\x8b\xe3\x83\x83\xe3\x82\xaf", "screenscraper: escapes");
        const SsMedia* cover = ssPickMedia(g, *ssKind("portada"), regions);
        check(cover && cover->region == "eu", "screenscraper: the cover of the ROM's region");
        const SsMedia* us = ssPickMedia(g, *ssKind("portada"), ssRegions("USA"));
        check(us && us->url.find("devpassword") == std::string::npos && us->url.find("sspassword") == std::string::npos &&
                  us->url.find("jeuid=3&media=box-2D(us)") != std::string::npos,
              "screenscraper: the accounts are not kept with the media");
        check(ssPickMedia(g, *ssKind("logo"), regions) != nullptr, "screenscraper: a second type for a kind");
        check(ssPickMedia(g, *ssKind("fanart"), regions) != nullptr, "screenscraper: media without a region");
        check(ssPickMedia(g, *ssKind("etiqueta"), regions) == nullptr, "screenscraper: a kind it does not have");
        // What plumnin keeps reads back the same.
        const SsAnswer back = parseSsAnswer(200, ssGameJson(g));
        check(back.games.size() == 1 && back.games[0].id == 3 && back.games[0].names == g.names &&
                  back.games[0].medias.size() == g.medias.size() && back.games[0].genres == g.genres &&
                  back.games[0].synopsis == g.synopsis && back.games[0].players == "1",
              "screenscraper: the kept answer reads back");
    }
    check(a.requestsToday == 12 && a.maxRequests == 20000, "screenscraper: the day's requests");

    const SsAnswer search = parseSsAnswer(200, "{\"response\":{\"jeux\":[{\"id\":\"3\",\"noms\":[{\"region\":\"wor\",\"text\":\"A\"}]},"
                                               "{\"id\":\"5\",\"noms\":[{\"region\":\"wor\",\"text\":\"B\"}]}]}}");
    check(search.result == SsAnswer::Ok && search.games.size() == 2 && search.games[1].id == 5, "screenscraper: a search's games");
    check(parseSsAnswer(200, "{\"response\":{\"jeux\":[{}]}}").result == SsAnswer::NotFound, "screenscraper: an empty search");
    check(parseSsAnswer(404, "Erreur : Rom/Iso/Dossier non trouvée !  ").result == SsAnswer::NotFound, "screenscraper: not found");
    check(parseSsAnswer(403, "Erreur de login : Verifier vos identifiants developpeur !").result == SsAnswer::Login,
          "screenscraper: a wrong account");
    check(parseSsAnswer(430, "Votre quota de scrape est dépassé pour aujourd'hui !").result == SsAnswer::Quota,
          "screenscraper: the day's quota");
    check(parseSsAnswer(429, "The maximum threads allowed to leecher users is already used").result == SsAnswer::Busy,
          "screenscraper: too busy");
    check(parseSsAnswer(0, "").result == SsAnswer::Offline, "screenscraper: no connection");
    // A good answer is not taken for an error by the words in it (the servers' "threadsmin", a synopsis...).
    const SsAnswer words = parseSsAnswer(200, "{\"header\":{\"success\":\"true\",\"error\":\"\"},\"response\":{"
                                              "\"serveurs\":{\"threadsmin\":\"1\"},\"ssuser\":{\"requeststoday\":\"3\","
                                              "\"maxrequestsperday\":\"20000\"},\"jeu\":{\"id\":\"9\",\"synopsis\":[{\"langue\":"
                                              "\"fr\",\"text\":\"Un quota d'identifiants non trouvés.\"}]}}}");
    check(words.result == SsAnswer::Ok && words.games.size() == 1 && words.requestsToday == 3,
          "screenscraper: words in a good answer");
    check(parseSsAnswer(200, "{\"header\":{\"success\":\"false\",\"error\":\"Votre quota de scrape est dépassé\"}}").result ==
              SsAnswer::Quota,
          "screenscraper: a JSON answer that says it failed");

    SsCredentials c{"dev id", "p&w", "plumnin", "me", "secret"};
    const std::string url = ssUrl(c, "jeuInfos.php", ssRomParams(2, "Zelda (Europe).zip", 131088, 0xF9394E97u, false));
    check(url == "https://api.screenscraper.fr/api2/jeuInfos.php?devid=dev%20id&devpassword=p%26w&softname=plumnin&output=json"
                 "&ssid=me&sspassword=secret&systemeid=3&romtype=rom&romnom=Zelda%20%28Europe%29.zip&romtaille=131088&crc=F9394E97",
          "screenscraper: a ROM's request");
    check(ssRomParams(0, "Tetris.gb", 0, 0, false).rfind("systemeid=9&", 0) == 0 &&
              ssRomParams(1, "a.gbc", 0, 0, false).rfind("systemeid=10&", 0) == 0 &&
              ssRomParams(3, "a.sfc", 0, 0, false).rfind("systemeid=4&", 0) == 0,
          "screenscraper: each console's number");
    check(ssWithCredentials("https://a.screenscraper.fr/api2/mediaJeu.php?devid=old&jeuid=3", c) ==
              "https://a.screenscraper.fr/api2/mediaJeu.php?devid=dev%20id&devpassword=p%26w&softname=plumnin&ssid=me&sspassword=secret&jeuid=3",
          "screenscraper: a medium's accounts");
    check(ssWithCredentials("https://example.com/x.png?a=1", c) == "https://example.com/x.png?a=1",
          "screenscraper: other servers get no accounts");
    const auto regions = ssRegions("USA · Europe");
    check(regions.size() > 3 && regions[0] == "us" && regions[1] == "eu" && regions[2] == "wor", "screenscraper: regions in order");
    JsonValue j;
    check(!parseJson("Erreur", j) && parseJson("[1, \"a\", {\"b\": null},]", j) && j.items.size() == 3, "screenscraper: JSON");
}

void testLibraryNames() {
    check(gameTitle("Lion King, The (World)") == "The Lion King", "library: trailing ', The' moves to the front");
    check(gameTitle("Adventures of Batman & Robin, The (USA)") == "The Adventures of Batman & Robin",
          "library: article with an ampersand in the name");
    check(gameTitle("Sonic The Hedgehog 2 (World) (Rev B)") == "Sonic The Hedgehog 2", "library: tags dropped");
    check(gameTitle("Golden Axe III") == "Golden Axe III", "library: name without tags kept");
    check(gameTags("Sonic The Hedgehog 2 (World) (Rev B)") == "World · Rev B", "library: tags listed");
    const auto names = coverNames("Comix Zone (USA) (Sega Smash Pack)");
    check(names.size() > 2 && names[0] == "Comix Zone (USA) (Sega Smash Pack)" && names[1] == "Comix Zone (USA)",
          "library: cover names drop trailing tags one by one");
    check(coverNames("Sonic & Knuckles (World)")[0] == "Sonic _ Knuckles (World)", "library: '&' becomes '_' like libretro");
    const auto bare = coverNames("Golden Axe III");
    check(!bare.empty() && bare[0] == "Golden Axe III (USA)", "library: a bare name tries the usual regions");
    check(urlEncode("Columns (USA, Europe)") == "Columns%20%28USA%2C%20Europe%29", "library: URL encoding");
    check(searchKey("Dr. Robotnik's  Mean-Bean") == "dr robotnik s mean bean", "library: search key");

    const std::string html = "<a href=\"../\">../</a>\n<a href=\"Sonic%20The%20Hedgehog%202%20%28World%29.png\">Sonic...</a>\n"
                             "<a href=\"Ecco%20the%20Dolphin%20%28USA%29.png\">Ecco</a>\n<a href=\"Tools.txt\">x</a>";
    const auto fromHtml = parseCoverList(html);
    check(fromHtml.size() == 2 && fromHtml[0] == "Ecco the Dolphin (USA)" && fromHtml[1] == "Sonic The Hedgehog 2 (World)",
          "library: cover list from libretro's index");
    const std::string json = "{\"tree\":[{\"path\": \"Pok\\u00e9mon (Japan).png\",\"type\":\"blob\"},"
                             "{\"path\":\"Golden Axe II (World).png\"}]}";
    const auto fromJson = parseCoverList(json);
    check(fromJson.size() == 2 && fromJson[0] == "Golden Axe II (World)" && fromJson[1] == "Pok\xc3\xa9mon (Japan)",
          "library: cover list from GitHub's tree");
    const std::vector<std::string> set = {"Golden Axe (World)", "Golden Axe II (World)", "Golden Axe III (Japan)",
                                          "Sonic The Hedgehog (USA, Europe)", "Sonic The Hedgehog 2 (World)",
                                          "Sonic The Hedgehog 3 (USA)", "Streets of Rage 2 (USA)", "Lion King, The (World)"};
    auto similar = similarCoverNames(set, "Golden Axe 2", 3);
    check(!similar.empty() && similar[0] == "Golden Axe II (World)", "library: '2' finds the 'II' cover first");
    similar = similarCoverNames(set, "sonic hedgehod 3", 2);
    check(!similar.empty() && similar[0] == "Sonic The Hedgehog 3 (USA)", "library: a typo still finds the cover");
    similar = similarCoverNames(set, "The Lion King (Europe)", 5);
    check(!similar.empty() && similar[0] == "Lion King, The (World)", "library: ', The' and other regions");
    check(similarCoverNames(set, "Columns", 5).empty(), "library: nothing alike, no suggestions");

    const std::string tree = "{\"tree\":[{\"path\":\"Sonic The Hedgehog (USA, Europe).cht\"},"
                             "{\"path\":\"Sonic The Hedgehog 2 (World) (Rev A).cht\"},{\"path\":\"Sonic The Hedgehog 2 (World).cht\"},"
                             "{\"path\":\"Sonic The Hedgehog 2 (World) (Action Replay).cht\"},{\"path\":\"cover.png\"}]}";
    const auto chts = parseFileList(tree, ".cht");
    check(chts.size() == 4 && std::is_sorted(chts.begin(), chts.end()), "library: cheat lists from GitHub's tree");
    auto lists = cheatFileNames(chts, "Sonic The Hedgehog 2 (World)", 5);
    check(lists.size() == 4 && lists[0] == "Sonic The Hedgehog 2 (World)" &&
              lists[1] == "Sonic The Hedgehog 2 (World) (Action Replay)",
          "library: the cheat list named like the ROM first, then its Action Replay one, then similar ones");
    lists = cheatFileNames(chts, "Sonic The Hedgehog 2 (Europe)", 5);
    check(!lists.empty() && lists[0] == "Sonic The Hedgehog 2 (World)", "library: another region's cheat list");
    check(cheatFileNames(chts, "Columns (World)", 5).empty(), "library: no cheat lists for an unknown game");
}

// A network between two netplay sessions, in memory: each packet arrives `delay`
// milliseconds later (plus some jitter, so some overtake others), and some are lost
// or arrive twice.
struct SimLink {
    struct Packet {
        uint32_t at;
        std::vector<uint8_t> data;
    };
    std::vector<Packet> toA, toB;
    std::mt19937 rng{1234};
    uint32_t now = 0;
    uint32_t delay = 40;
    int lossPercent = 10;

    void post(std::vector<Packet>& q, const uint8_t* d, size_t n) {
        if (static_cast<int>(rng() % 100) < lossPercent) return;
        const int copies = rng() % 20 == 0 ? 2 : 1;
        for (int i = 0; i < copies; ++i)
            q.push_back({now + delay + static_cast<uint32_t>(rng() % 30), std::vector<uint8_t>(d, d + n)});
    }
    static bool take(std::vector<Packet>& q, uint32_t now, std::vector<uint8_t>& out) {
        for (size_t i = 0; i < q.size(); ++i)
            if (q[i].at <= now) {
                out = std::move(q[i].data);
                q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        return false;
    }
};

struct SimEnd : NetTransport {
    SimLink& link;
    bool isA;
    SimEnd(SimLink& l, bool a) : link(l), isA(a) {}
    void send(const uint8_t* d, size_t n) override { link.post(isA ? link.toB : link.toA, d, n); }
    bool receive(std::vector<uint8_t>& p) override { return SimLink::take(isA ? link.toA : link.toB, link.now, p); }
};

// What each player presses on their n-th frame: a new random pad every few frames.
PadState scriptedPad(int player, uint32_t n) {
    std::mt19937 rng(static_cast<uint32_t>(player * 7919 + n / 5));
    return unpackPad(static_cast<uint16_t>(rng() & 0xFFF));
}


void testNetplay() {
    check(packPad(unpackPad(0xA5C)) == 0xA5C && packPad(PadState{}) == 0, "netplay: los mandos caben en 12 bits");

    constexpr uint32_t kFrames = 400;
    NetplaySession::Config config;
    config.inputDelay = 2;
    config.maxRollback = 8;
    // Without the network: frame f has the pads pressed on frame f - inputDelay.
    std::vector<uint32_t> reference;
    {
        auto c = testConsole();
        for (int i = 0; i < 30; ++i) c->runFrame();  // the host played a little alone first
        for (uint32_t f = 0; f < kFrames; ++f) {
            const bool early = f < static_cast<uint32_t>(config.inputDelay);
            const uint32_t n = early ? 0 : f - static_cast<uint32_t>(config.inputDelay);
            c->setPads(early ? PadState{} : scriptedPad(0, n), early ? PadState{} : scriptedPad(1, n));
            c->runFrame();
            reference.push_back(frameHash(c->frame()));
        }
    }

    SimLink link;
    SimEnd hostEnd(link, true), guestEnd(link, false);
    NetplaySession host(hostEnd, NetplaySession::Role::Host, config);
    NetplaySession guest(guestEnd, NetplaySession::Role::Guest, config);
    auto a = testConsole(), b = testConsole();
    for (int i = 0; i < 30; ++i) a->runFrame();
    b->setPadType(1, PadType::None);  // the host's pad types win
    std::vector<uint32_t> hashA(kFrames), hashB(kFrames);
    host.onFrame = [&](uint32_t f, Console& c) { if (f < kFrames) hashA[f] = frameHash(c.frame()); };
    guest.onFrame = [&](uint32_t f, Console& c) { if (f < kFrames) hashB[f] = frameHash(c.frame()); };
    int rollbacks = 0, stalls = 0;
    for (int step = 0; step < 40000; ++step) {
        link.now = static_cast<uint32_t>(step);  // one millisecond per step
        // The two displays tick at slightly different rates, as two monitors do.
        const bool hostTick = step % 16 == 0, guestTick = step % 17 == 3;
        host.poll(*a, link.now);
        guest.poll(*b, link.now);
        if (hostTick && host.frame() < kFrames) {
            if (!host.advance(*a, scriptedPad(0, host.frame()))) ++stalls;
            rollbacks += host.lastRollback() > 0;
        }
        if (guestTick && guest.frame() < kFrames) {
            if (!guest.advance(*b, scriptedPad(1, guest.frame()))) ++stalls;
            rollbacks += guest.lastRollback() > 0;
        }
        if (host.frame() >= kFrames) host.catchUp(*a);
        if (guest.frame() >= kFrames) guest.catchUp(*b);
        if (host.confirmedFrames() >= kFrames && guest.confirmedFrames() >= kFrames) break;
    }
    check(host.phase() == NetplaySession::Phase::Playing && guest.phase() == NetplaySession::Phase::Playing,
          "netplay: el invitado se conecta y los dos juegan");
    check(host.confirmedFrames() >= kFrames && guest.confirmedFrames() >= kFrames,
          "netplay: los dos confirman todos los fotogramas pese a perder paquetes");
    check(rollbacks > 0, "netplay: hay vueltas atrás cuando el mando del otro llega tarde");
    check(hashA == hashB, "netplay: las dos consolas ven lo mismo en cada fotograma");
    check(hashA == reference, "netplay: igual que jugando los dos en la misma consola");
    check(host.desyncFrame() < 0 && guest.desyncFrame() < 0, "netplay: no se detecta ningún descompás");
    check(host.pingMs() > 60 && host.pingMs() < 140, "netplay: el ping se mide (unos 2 x 40 ms)");
    std::printf("     %d vueltas atrás, %d esperas, ping %d ms\n", rollbacks, stalls, host.pingMs());

    guest.end();
    for (uint32_t t = 0; t < 200; ++t) {
        link.now += 1;
        host.poll(*a, link.now);
    }
    check(host.phase() == NetplaySession::Phase::Ended && host.endReason().find("salido") != std::string::npos,
          "netplay: el anfitrión se entera de que el invitado se va");

    // A guest with another game is turned away with a reason.
    SimLink link2;
    link2.lossPercent = 0;
    SimEnd h2(link2, true), g2(link2, false);
    NetplaySession host2(h2, NetplaySession::Role::Host, config);
    NetplaySession guest2(g2, NetplaySession::Role::Guest, config);
    auto c = testConsole(), d = testConsole(ConsoleSystem::Nes, 9);
    for (int step = 0; step < 2000 && guest2.phase() != NetplaySession::Phase::Ended; ++step) {
        link2.now = static_cast<uint32_t>(step);
        host2.poll(*c, link2.now);
        guest2.poll(*d, link2.now);
    }
    check(guest2.phase() == NetplaySession::Phase::Ended && guest2.endReason().find("otro juego") != std::string::npos &&
              host2.phase() == NetplaySession::Phase::Waiting,
          "netplay: un invitado con otro juego no entra y el anfitrión sigue esperando");
}

// RetroAchievements: the game's hash, its memory as the achievements address it, and a
// whole session (login, game, an achievement earned) against a made-up server.
void testAchievements() {
    TestConsole c;
    c.loadRomData(std::vector<uint8_t>(0x200, 0));
    check(Achievements::gameHash(c) == "bf619eac0cdf3f68d496ea9344137e8b", "logros: hash = MD5 de la ROM");
    std::vector<MemoryRegion> map = c.memoryMap();
    uint8_t* ram = map.empty() ? nullptr : map[0].data;
    if (!ram) {
        check(false, "logros: la consola da su memoria");
        return;
    }
    ram[0x10] = 0x12;
    ram[0x11] = 0x34;
    ram[0xFF] = 0x56;
    uint8_t bytes[3] = {};
    check(Achievements::readMemory(c, 0x10, bytes, 2) == 2 && bytes[0] == 0x12 && bytes[1] == 0x34,
          "logros: la memoria como la da la consola");
    check(Achievements::readMemory(c, 0xFF, bytes, 2) == 1 && bytes[0] == 0x56, "logros: nada más allá de su mapa");

    std::vector<std::string> asked;
    auto server = [&](const std::string& url, const std::string& post, const std::string& agent, std::string& body) {
        asked.push_back(post);
        check(agent.rfind("plumnin/", 0) == 0 && agent.find("rcheevos/") != std::string::npos,
              "logros: el User-Agent dice plumnin y rcheevos");
        (void)url;
        if (post.rfind("r=login2", 0) == 0)
            body = "{\"Success\":true,\"User\":\"Probador\",\"Token\":\"FICHA\",\"Score\":100,\"SoftcoreScore\":0,"
                   "\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
        else if (post.rfind("r=achievementsets", 0) == 0)
            body = "{\"Success\":true,\"GameId\":7,\"Title\":\"Juego de prueba\",\"ConsoleId\":7,"
                   "\"ImageIconUrl\":\"http://servidor/juego.png\",\"RichPresenceGameId\":7,\"RichPresencePatch\":\"\","
                   "\"Sets\":[{\"AchievementSetId\":70,\"GameId\":7,\"Title\":null,\"Type\":\"core\","
                   "\"ImageIconUrl\":\"http://servidor/juego.png\",\"Achievements\":["
                   "{\"ID\":701,\"Title\":\"Cinco\",\"Description\":\"Llega a cinco\",\"Flags\":3,\"Points\":10,"
                   "\"MemAddr\":\"0xH0010=5\",\"Author\":\"Alguien\",\"BadgeName\":\"00001\","
                   "\"Created\":1367266583,\"Modified\":1376929305}],\"Leaderboards\":[]}]}";
        else if (post.rfind("r=startsession", 0) == 0)
            body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
        else if (post.rfind("r=awardachievement", 0) == 0)
            body = "{\"Success\":true,\"Score\":110,\"SoftcoreScore\":0,\"AchievementID\":701,\"AchievementsRemaining\":0}";
        else
            body = "{\"Success\":true}";
        return 200;
    };
    Achievements ra(server, false);
    ra.loadGame(c, Achievements::gameHash(c));  // before logging in: it waits for it
    ra.loginWithPassword("Probador", "secreta");
    for (int i = 0; i < 6; ++i) ra.poll();
    check(ra.loggedIn() && ra.token() == "FICHA", "logros: sesión iniciada, con la ficha para la próxima vez");
    check(ra.gameLoaded() && ra.gameTitle() == "Juego de prueba" && ra.achievementCount() == 1,
          "logros: el juego identificado por su hash, con su logro");
    bool gameNotice = false;
    for (const auto& n : ra.takeNotices()) gameNotice |= n.kind == Achievements::Notice::Kind::GameStarted;
    check(gameNotice, "logros: aviso de juego con logros");

    ram[0x10] = 0;
    ra.doFrame();
    ra.doFrame();
    check(ra.takeNotices().empty(), "logros: nada mientras no se cumple");
    ram[0x10] = 5;
    ra.doFrame();
    const auto notices = ra.takeNotices();
    check(notices.size() == 1 && notices[0].kind == Achievements::Notice::Kind::Unlocked && notices[0].title == "Cinco" &&
              notices[0].points == 10,
          "logros: logro conseguido al cumplirse");
    ra.poll();
    check(!asked.empty() && asked.back().rfind("r=awardachievement", 0) == 0 && asked.back().find("a=701") != std::string::npos,
          "logros: el logro se envía al servidor");
    check(ra.unlockedCount() == 1, "logros: cuenta como conseguido");

    // Savestates keep the progress; an empty one starts over.
    const std::vector<uint8_t> progress = ra.saveProgress();
    check(!progress.empty(), "logros: el progreso cabe en el estado");
    ra.unloadGame();
    check(!ra.gameLoaded(), "logros: juego cerrado");
}

void testTextureCache() {
    const int w = 61, h = 37;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    uint32_t seed = 12345;
    for (size_t i = 0; i < px.size(); ++i) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t kind = (seed >> 16) % 6;
        const uint32_t prev = i ? px[i - 1] : 0xFF000000u;
        if (kind == 0) px[i] = prev;                                   // a run
        else if (kind == 1 && i > 20) px[i] = px[i - 17];              // probably in the index
        else if (kind == 2) px[i] = (prev & 0xFF000000u) | ((prev + 0x010101u) & 0xFFFFFFu);  // a small step
        else if (kind == 3) px[i] = (prev & 0xFF000000u) | ((prev + 0x0A0C0Eu) & 0xFFFFFFu);  // a larger one
        else if (kind == 4) px[i] = (seed & 0x00FFFFFFu) | (((seed >> 8) & 1) ? 0x80000000u : 0xFF000000u);
        else px[i] = seed;
    }
    for (size_t i = 0; i < 100; ++i) px[500 + i] = 0xFF336699u;  // longer than a run can say
    const std::vector<uint8_t> qoi = TextureCache::encodeQoi(px, w, h);
    std::vector<uint32_t> back;
    int bw = 0, bh = 0;
    check(TextureCache::decodeQoi(qoi.data(), qoi.size(), back, &bw, &bh) && bw == w && bh == h && back == px,
          "texture cache: QOI gives back the same pixels");
    check(!TextureCache::decodeQoi(qoi.data(), qoi.size() / 2, back, &bw, &bh), "texture cache: a cut QOI is refused");

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "plumnin-texture-cache-test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string source = (dir / "cover.png").u8string();
    { std::ofstream(fs::u8path(source), std::ios::binary) << "not really a png"; }
    TextureCache cache;
    cache.setFolder((dir / "cache").u8string() + "/");
    const uint64_t key = TextureCache::key(source, 7);
    check(key != 0 && key != TextureCache::key(source, 8) && TextureCache::key((dir / "none.png").u8string(), 7) == 0,
          "texture cache: the key depends on the file and how it was shrunk");
    check(!cache.load(key, back, &bw, &bh), "texture cache: nothing cached yet");
    cache.store(key, px, w, h);
    back.clear();
    check(cache.load(key, back, &bw, &bh) && back == px, "texture cache: read back from disk");
    cache.prune({key});
    check(cache.load(key, back, &bw, &bh), "texture cache: prune keeps what is used");
    cache.prune({});
    check(!cache.load(key, back, &bw, &bh), "texture cache: prune deletes what is not");
    fs::remove_all(dir, ec);
}


}  // namespace

void testGameBoy() {
    check(emulated(ConsoleSystem::GameBoy) && emulated(ConsoleSystem::GameBoyColor), "game boy: emulated");
    auto gb = std::make_unique<GameBoy>();
    std::string error;
    check(gb->loadRomData(gameBoyProgram(), &error) && gb->system() == ConsoleSystem::GameBoy &&
              gb->title() == "PLUMNIN TEST" && !gb->hasSaveData() && gb->ports() == 1,
          "game boy: a cartridge with only ROM");
    std::vector<uint8_t> unknown = gameBoyProgram(0xFC);
    check(!GameBoy().loadRomData(unknown, &error) && error.find("FC") != std::string::npos,
          "game boy: a cartridge chip it does not emulate is refused");
    for (int i = 0; i < 10; ++i) gb->runFrame();
    check(gb->peek(0xFE00) == 0x50 && gb->peek(0xFE01) == 0x50, "game boy: the program put its sprite in OAM");
    PadState right;
    right.right = true;
    gb->setPad(0, right);
    for (int i = 0; i < 20; ++i) gb->runFrame();
    const int x = gb->peek(0xFE01), y = gb->peek(0xFE00);
    check(x >= 0x50 + 19 && x <= 0x50 + 21 && y == 0x50, "game boy: the cross moves the sprite");
    gb->setPad(0, PadState{});
    gb->runFrame();
    const VideoFrame& f = gb->frame();
    const int sx = gb->peek(0xFE01) - 8 + 3, sy = gb->peek(0xFE00) - 16 + 3;
    check(f.width == 160 && f.height == 144 && f.pixel(sx, sy) == 0xFF081820u &&
              f.info[static_cast<size_t>(sy) * VideoFrame::kMaxWidth + sx].layer == Layer::Sprite,
          "game boy: the sprite is drawn darkest, on the sprite layer");
    int back = 0, clear = 0;
    for (int i = 0; i < 160; ++i) {
        const size_t at = static_cast<size_t>(10) * VideoFrame::kMaxWidth + i;
        if (f.planeB[at] >> 24) ++back;
        else ++clear;
    }
    check(back == 80 && clear == 80 && f.lines[10].displayEnabled, "game boy: the checkerboard is on the back plane, colour 0 clear");
    const int16_t scroll = f.lines[10].hscrollB;
    gb->runFrame();
    check(gb->frame().lines[10].hscrollB == scroll - 1, "game boy: the scroll goes into the frame");

    gb->audio().clearSamples();
    gb->setSampleRate(48000);
    PadState a;
    a.a = true;
    gb->setPad(0, a);
    for (int i = 0; i < 5; ++i) gb->runFrame();
    const std::vector<int16_t>& samples = gb->audio().samples();
    int loud = 0;
    for (int16_t v : samples) loud = std::max(loud, std::abs(static_cast<int>(v)));
    check(samples.size() > 7900 && samples.size() < 8200 && loud > 1000, "game boy: A beeps, at the asked rate");
    gb->setPad(0, PadState{});

    // A state brings back the same frames and sound.
    std::vector<uint8_t> state;
    check(gb->saveState(state), "game boy: savestate");
    auto runSome = [&](GameBoy& g) {
        uint32_t h = 2166136261u;
        PadState p;
        for (int i = 0; i < 40; ++i) {
            p.left = i < 10;
            p.down = i > 20;
            p.a = i % 7 == 0;
            g.setPad(0, p);
            g.audio().clearSamples();
            g.runFrame();
            for (int yy = 0; yy < 144; ++yy)
                for (int xx = 0; xx < 160; ++xx) h = (h ^ g.frame().pixel(xx, yy)) * 16777619u;
            for (int16_t v : g.audio().samples()) h = (h ^ static_cast<uint16_t>(v)) * 16777619u;
        }
        return h;
    };
    const uint32_t first = runSome(*gb);
    check(gb->loadState(state, &error) && runSome(*gb) == first, "game boy: a savestate replays the same frames and sound");
    GameBoy other;
    other.loadRomData(gameBoyProgram(), &error);
    other.setSampleRate(48000);
    check(other.loadState(state, &error) && runSome(other) == first, "game boy: and in another console");

    // Cheats: a Game Genie code patches what the ROM answers, a GameShark one RAM.
    CheatPatch p;
    const CheatDecoder decode = cheatDecoderFor(ConsoleSystem::GameBoy);
    check(decode && decode("00A-17B-C49", &p, &error) && p.address == 0x4A17 && p.value == 0 && p.compare == 0xC8 && !p.ram,
          "game boy: a Game Genie code");
    check(decode("01AAC0C0", &p, &error) && p.ram && p.address == 0xC0C0 && p.value == 0xAA, "game boy: a GameShark code");
    check(!decode("12345", &p, &error) && !decode("01AAC0CG", &p, &error), "game boy: wrong codes");
    std::vector<CheatPatch> patches;
    check(decodeCheatCodes("121-51F+01AAC0C0", decode, &patches, &error) && patches.size() == 2, "game boy: two codes");
    gb->setCheats(patches);
    gb->runFrame();
    check(gb->peek(0x0151) == 0x12 && gb->peek(0xC0C0) == 0xAA, "game boy: the cheats are on");
    gb->setCheats({});
    check(gb->peek(0x0151) == 0x31, "game boy: and off");

    check(gb->achievementsConsole() == 4 && !gb->memoryMap().empty() && gb->memoryMap()[0].address == 0,
          "game boy: the achievements' memory map");

    // A battery-backed MBC3 with its clock: the save is the RAM and 48 bytes of clock.
    GameBoy mbc3;
    check(mbc3.loadRomData(gameBoyProgram(0x10, 0x03), &error) && mbc3.hasSaveData(), "game boy: an MBC3 with battery");
    std::vector<uint8_t> save = mbc3.saveData();
    check(save.size() == 0x8000 + 48, "game boy: its save carries the clock");
    save[5] = 0x42;
    mbc3.loadSaveData(save);
    check(mbc3.saveData()[5] == 0x42, "game boy: the save loads back");

    auto color = createConsole(ConsoleSystem::GameBoyColor);
    std::vector<uint8_t> cgb = gameBoyProgram();
    cgb[0x143] = 0x80;
    check(color && color->loadRomData(cgb, &error) && color->system() == ConsoleSystem::GameBoyColor &&
              color->achievementsConsole() == 6,
          "game boy: a game for the Color runs on a Color");
    for (int i = 0; i < 10; ++i) color->runFrame();
    check(color->frame().pixel(0, 0) == 0xFFFFFFFFu, "game boy: the Color's palettes start white");
}

void testNes() {
    check(emulated(ConsoleSystem::Nes), "nes: emulated");
    auto nes = std::make_unique<Nes>();
    std::string error;
    check(nes->loadRomData(nesProgram(), &error) && nes->system() == ConsoleSystem::Nes && !nes->hasSaveData() &&
              nes->ports() == 2 && nes->effectiveRegion() == Region::UsaNtsc,
          "nes: an NROM cartridge");
    std::vector<uint8_t> unknown = nesProgram();
    unknown[6] = 0x50;  // mapper 5, the MMC5
    check(!Nes().loadRomData(unknown, &error) && error.find("5") != std::string::npos,
          "nes: a mapper it does not emulate is refused");
    check(!Nes().loadRomData({'N', 'E', 'S', 0x1A, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, &error),
          "nes: a cut ROM is refused");
    for (int i = 0; i < 10; ++i) nes->runFrame();
    check(nes->peek(0x0200) == 0x50 && nes->peek(0x0203) == 0x50, "nes: the program put its sprite in OAM");
    PadState right;
    right.right = true;
    nes->setPad(0, right);
    for (int i = 0; i < 20; ++i) nes->runFrame();
    const int x = nes->peek(0x0203);
    check(x >= 0x50 + 19 && x <= 0x50 + 21, "nes: the cross moves the sprite");
    nes->setPad(0, PadState{});
    nes->runFrame();
    nes->runFrame();
    const VideoFrame& f = nes->frame();
    // The sprite's OAM y is a line above it; the frame leaves out the first 8 lines.
    const int sx = nes->peek(0x0203) + 3, sy = 0x50 + 1 - 8 + 3;
    const size_t at = static_cast<size_t>(sy) * VideoFrame::kMaxWidth + static_cast<size_t>(sx);
    check(f.width == 256 && f.height == 224 && f.pixel(sx, sy) == 0xFFB53120u && f.info[at].layer == Layer::Sprite &&
              f.spriteInfo[at].priority,
          "nes: the sprite is drawn red, on the sprite layer");
    int back = 0, clear = 0;
    for (int i = 0; i < 256; ++i) {
        const size_t p = static_cast<size_t>(100) * VideoFrame::kMaxWidth + static_cast<size_t>(i);
        if (f.planeB[p] >> 24) ++back;
        else ++clear;
    }
    check(back == 128 && clear == 128 && f.lines[100].displayEnabled && f.backdrop[100] == 0xFF000000u,
          "nes: the stripes are on the back plane, colour 0 clear");
    const int16_t scroll = f.lines[100].hscrollB;
    nes->runFrame();
    check(nes->frame().lines[100].hscrollB == scroll - 1, "nes: the scroll goes into the frame");

    nes->audio().clearSamples();
    nes->setSampleRate(48000);
    PadState a;
    a.a = true;
    nes->setPad(0, a);
    for (int i = 0; i < 6; ++i) nes->runFrame();
    const std::vector<int16_t>& samples = nes->audio().samples();
    int loud = 0;
    for (int16_t v : samples) loud = std::max(loud, std::abs(static_cast<int>(v)));
    check(samples.size() > 9400 && samples.size() < 9800 && loud > 1000, "nes: A beeps, at the asked rate");
    nes->setPad(0, PadState{});

    // A state brings back the same frames and sound.
    std::vector<uint8_t> state;
    check(nes->saveState(state), "nes: savestate");
    auto runSome = [&](Nes& n) {
        uint32_t h = 2166136261u;
        PadState p;
        for (int i = 0; i < 40; ++i) {
            p.left = i < 10;
            p.right = i > 20;
            p.a = i % 7 == 0;
            n.setPad(0, p);
            n.audio().clearSamples();
            n.runFrame();
            for (int yy = 0; yy < 224; ++yy)
                for (int xx = 0; xx < 256; ++xx) h = (h ^ n.frame().pixel(xx, yy)) * 16777619u;
            for (int16_t v : n.audio().samples()) h = (h ^ static_cast<uint16_t>(v)) * 16777619u;
        }
        return h;
    };
    const uint32_t first = runSome(*nes);
    check(nes->loadState(state, &error) && runSome(*nes) == first, "nes: a savestate replays the same frames and sound");
    Nes other;
    other.loadRomData(nesProgram(), &error);
    other.setSampleRate(48000);
    check(other.loadState(state, &error) && runSome(other) == first, "nes: and in another console");

    // Cheats: a Game Genie code patches what the ROM answers, a Pro Action Replay one RAM.
    CheatPatch p;
    const CheatDecoder decode = cheatDecoderFor(ConsoleSystem::Nes);
    check(decode && decode("GOSSIP", &p, &error) && p.address == 0xD1DD && p.value == 0x14 && p.compare < 0 && !p.ram,
          "nes: a 6-letter Game Genie code");
    check(decode("ZEXPYGLA", &p, &error) && p.address == 0x94A7 && p.value == 0x02 && p.compare == 0x03,
          "nes: an 8-letter Game Genie code");
    check(decode("0075:09", &p, &error) && p.ram && p.address == 0x75 && p.value == 9, "nes: a Pro Action Replay code");
    check(!decode("GOSSI", &p, &error) && !decode("2002:00", &p, &error) && !decode("12345G", &p, &error),
          "nes: wrong codes");
    std::vector<CheatPatch> patches;
    check(decodeCheatCodes("AAAAAA+0300:2A", decode, &patches, &error) && patches.size() == 2, "nes: two codes");
    nes->setCheats(patches);
    nes->runFrame();
    check(nes->peek(0x0300) == 0x2A, "nes: the RAM cheat is on");
    nes->setCheats({});

    check(nes->achievementsConsole() == 7 && nes->memoryMap().size() >= 4 && nes->memoryMap()[0].address == 0 &&
              nes->memoryMap()[0].size == 0x800,
          "nes: the achievements' memory map");

    // A battery-backed cartridge: the save is its PRG RAM.
    Nes saved;
    check(saved.loadRomData(nesProgram(true), &error) && saved.hasSaveData() && saved.saveData().size() == 0x2000,
          "nes: a cartridge with battery");
    std::vector<uint8_t> save = saved.saveData();
    save[5] = 0x42;
    saved.loadSaveData(save);
    check(saved.saveData()[5] == 0x42 && saved.peek(0x6005) == 0x42, "nes: the save loads back at 6000");

    // PAL: the header's flag, or the region chosen.
    std::vector<uint8_t> palRom = nesProgram();
    palRom[9] = 1;
    Nes pal;
    check(pal.loadRomData(palRom, &error) && pal.effectiveRegion() == Region::EuropePal && pal.refreshRate() < 51,
          "nes: a PAL cartridge runs at 50 Hz");
    nes->setRegion(Region::EuropePal);
    check(nes->effectiveRegion() == Region::EuropePal && nes->refreshRate() < 51, "nes: the region can be chosen");
    for (int i = 0; i < 5; ++i) nes->runFrame();
    check(nes->peek(0x0200) == 0x50, "nes: and the game runs again on it");
}

void testSnes() {
    check(emulated(ConsoleSystem::Snes), "snes: emulated");
    auto snes = std::make_unique<Snes>();
    std::string error;
    check(snes->loadRomData(snesProgram(), &error) && snes->system() == ConsoleSystem::Snes && !snes->hasSaveData() &&
              snes->title() == "PLUMNIN TEST" && snes->effectiveRegion() == Region::UsaNtsc && snes->loadNotice().empty(),
          "snes: a LoROM cartridge");
    check(!Snes().loadRomData(std::vector<uint8_t>(0x1000, 0), &error) && !error.empty(), "snes: a cut ROM is refused");
    std::vector<uint8_t> copier(512, 0);
    const std::vector<uint8_t> plain = snesProgram();
    copier.insert(copier.end(), plain.begin(), plain.end());
    check(Snes().loadRomData(copier, &error), "snes: a copier's header is skipped");
    for (int i = 0; i < 10; ++i) snes->runFrame();
    check(snes->peek(0x7E0010) == 0x50 && snes->cpuRegs().e == false, "snes: the program runs in native mode");
    PadState right;
    right.right = true;
    snes->setPad(0, right);
    for (int i = 0; i < 20; ++i) snes->runFrame();
    const int x = snes->peek(0x10);
    check(x >= 0x50 + 19 && x <= 0x50 + 21, "snes: the cross moves the sprite");
    snes->setPad(0, PadState{});
    snes->runFrame();
    snes->runFrame();
    const VideoFrame& f = snes->frame();
    const int sx = snes->peek(0x10) + 3, sy = 0x50 + 3;
    const size_t at = static_cast<size_t>(sy) * VideoFrame::kMaxWidth + static_cast<size_t>(sx);
    check(f.width == 256 && f.height == 224 && f.pixel(sx, sy) == 0xFFFF0000u && f.info[at].layer == Layer::Sprite,
          "snes: the sprite is drawn red, on the sprite layer");
    int front = 0, clear = 0;
    for (int i = 0; i < 256; ++i) {
        const size_t p = static_cast<size_t>(100) * VideoFrame::kMaxWidth + static_cast<size_t>(i);
        if (f.planeA[p] >> 24) ++front;
        else ++clear;
    }
    check(front == 128 && clear == 128 && f.lines[100].displayEnabled && f.backdrop[100] == 0xFF000000u,
          "snes: the stripes are on the front plane, colour 0 clear");
    const int16_t scroll = f.lines[100].hscrollA;
    snes->runFrame();
    check(snes->frame().lines[100].hscrollA == scroll - 1, "snes: the scroll goes into the frame");

    snes->audio().clearSamples();
    snes->setSampleRate(48000);
    for (int i = 0; i < 6; ++i) snes->runFrame();
    const size_t samples = snes->audio().samples().size();
    check(samples > 9300 && samples < 9900, "snes: sound at the asked rate");

    // A state brings back the same frames and sound.
    std::vector<uint8_t> state;
    check(snes->saveState(state), "snes: savestate");
    auto runSome = [&](Snes& n) {
        uint32_t h = 2166136261u;
        PadState p;
        for (int i = 0; i < 40; ++i) {
            p.left = i < 10;
            p.right = i > 20;
            n.setPad(0, p);
            n.audio().clearSamples();
            n.runFrame();
            for (int yy = 0; yy < 224; ++yy)
                for (int xx = 0; xx < 256; ++xx) h = (h ^ n.frame().pixel(xx, yy)) * 16777619u;
            for (int16_t v : n.audio().samples()) h = (h ^ static_cast<uint16_t>(v)) * 16777619u;
        }
        return h;
    };
    const uint32_t first = runSome(*snes);
    check(snes->loadState(state, &error) && runSome(*snes) == first, "snes: a savestate replays the same frames and sound");
    Snes other;
    other.loadRomData(snesProgram(), &error);
    other.setSampleRate(48000);
    check(other.loadState(state, &error) && runSome(other) == first, "snes: and in another console");

    // Cheats: a Game Genie code patches what the ROM answers, a Pro Action Replay one RAM.
    CheatPatch p;
    const CheatDecoder decode = cheatDecoderFor(ConsoleSystem::Snes);
    check(decode && decode("DDDD-DDDD", &p, &error) && p.address == 0 && p.value == 0 && !p.ram &&
              decode("FDDD-DDDD", &p, &error) && p.value == 0x10,
          "snes: a Game Genie code");
    std::set<uint32_t> bits;
    for (int digit = 2; digit < 8; ++digit)
        for (int b = 0; b < 4; ++b) {
            std::string code = "DDDD-DDDD";
            code[static_cast<size_t>(digit < 4 ? digit : digit + 1)] = "F406"[b];  // the digits worth 1, 2, 4 and 8
            if (decode(code, &p, &error)) bits.insert(p.address);
        }
    check(bits.size() == 24, "snes: each of the Game Genie's address bits lands on its own");
    check(decode("7E0020:2A", &p, &error) && p.ram && p.address == 0x7E0020 && p.value == 0x2A &&
              decode("7E00202A", &p, &error) && p.ram,
          "snes: a Pro Action Replay code");
    check(decode("009ABCEA", &p, &error) && !p.ram && p.address == 0x009ABC, "snes: a Pro Action Replay code in the ROM");
    check(!decode("DDDD-DDDG", &p, &error) && !decode("7E002", &p, &error), "snes: wrong codes");
    snes->setCheats({{0x7E0020, 0x2A, true, -1, true}});
    snes->runFrame();
    check(snes->peek(0x0020) == 0x2A, "snes: the RAM cheat is on");
    snes->setCheats({});

    check(snes->achievementsConsole() == 3 && snes->memoryMap().size() == 1 && snes->memoryMap()[0].size == 0x20000,
          "snes: the achievements' memory map");

    Snes saved;
    check(saved.loadRomData(snesProgram(true), &error) && saved.hasSaveData() && saved.saveData().size() == 0x800,
          "snes: a cartridge with battery");
    std::vector<uint8_t> save = saved.saveData();
    save[5] = 0x42;
    saved.loadSaveData(save);
    check(saved.saveData()[5] == 0x42 && saved.peek(0x700005) == 0x42, "snes: the save loads back at 700000");

    Snes pal;
    check(pal.loadRomData(snesProgram(false, 0x02), &error) && pal.effectiveRegion() == Region::EuropePal &&
              pal.refreshRate() < 51,
          "snes: a PAL cartridge runs at 50 Hz");
    snes->setPad(0, PadState{});
    snes->setRegion(Region::EuropePal);
    check(snes->effectiveRegion() == Region::EuropePal && snes->refreshRate() < 51, "snes: the region can be chosen");
    for (int i = 0; i < 5; ++i) snes->runFrame();
    check(snes->peek(0x10) == 0x50, "snes: and the game runs again on it");

    std::vector<uint8_t> chip = snesProgram();
    chip[0x7FD6] = 0x15;  // ROM, a Super FX and battery
    check(Snes().loadRomData(chip, &error), "snes: a game with a chip still loads");
    Snes withChip;
    withChip.loadRomData(chip, &error);
    check(withChip.loadNotice().find("Super FX") != std::string::npos, "snes: and says which chip it lacks");
}

int main() {
    testSystems();
    testConsoles();
    testTestConsole();
    testStates();
    testRewindBuffer();
    testFramePacer();
    testRewindPlayback();
    testRunAhead();
    testPlanes();
    testEdgeDistance();
    testProfiles();
    testLiveCamera();
    testCheats();
    testLibraryNames();
    testScreenScraper();
    testGameInfo();
    testTextureCache();
    testNetplay();
    testAchievements();
    testGameBoy();
    testNes();
    testSnes();
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
