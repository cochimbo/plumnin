// Runs the NES test ROMs (https://github.com/christopherpow/nes-test-roms) on plumnin's
// NES and says which pass. They are not part of plumnin: clone that repository and give
// its folder.
//
//   nes-test-roms ROOT              the suite below; exit code 1 if one that should pass fails
//   nes-test-roms ROOT --all        also the ones not expected to pass yet
//   nes-test-roms --run ROM [--frames N] [--png OUT] [--pad BUTTONS] [--pal]   one ROM, to look at it
//
// A test passes in one of three ways, depending on how it reports:
//   status     blargg's newer ones: 0 at 6000 once DE B0 61 is at 6001 (0x81 asks for a reset)
//   f8         blargg's older ones: 1 in zero page F8 after a number of frames
//   screen     the words it prints on screen once done ("Passed", "PASSED", "$01" for the 2005 ones)

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "nes/nes.h"

using plumnin::Nes;

namespace {

enum class Kind { Status, F8, Screen };

struct Test {
    const char* path;
    Kind kind;
    int frames;
    bool expected = true;  // false: known to fail for now
    const char* pad = "";  // buttons held from the start (cpu_timing_test: B for every instruction)
    const char* words = "Passed";  // for Screen: any of these, split by '|'
    int submapper = -1;            // >= 0: the header is rewritten as NES 2.0 with this submapper
    bool pal = false;              // run on a PAL console, whatever the header says

};

const Test kTests[] = {
    // Processor.
    {"instr_test-v5/rom_singles/01-basics.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/02-implied.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/03-immediate.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/04-zero_page.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/05-zp_xy.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/06-absolute.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/07-abs_xy.nes", Kind::Status, 900},
    {"instr_test-v5/rom_singles/08-ind_x.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/09-ind_y.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/10-branches.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/11-stack.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/12-jmp_jsr.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/13-rts.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/14-rti.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/15-brk.nes", Kind::Status, 600},
    {"instr_test-v5/rom_singles/16-special.nes", Kind::Status, 600},
    {"instr_misc/rom_singles/01-abs_x_wrap.nes", Kind::Status, 600},
    {"instr_misc/rom_singles/02-branch_wrap.nes", Kind::Status, 600},
    {"instr_misc/rom_singles/03-dummy_reads.nes", Kind::Status, 600},
    {"instr_misc/rom_singles/04-dummy_reads_apu.nes", Kind::Status, 600},
    {"instr_timing/rom_singles/1-instr_timing.nes", Kind::Status, 1800},
    {"instr_timing/rom_singles/2-branch_timing.nes", Kind::Status, 600},
    {"cpu_timing_test6/cpu_timing_test.nes", Kind::Screen, 1200, true, "B", "PASSED"},
    {"branch_timing_tests/1.Branch_Basics.nes", Kind::F8, 300},
    {"branch_timing_tests/2.Backward_Branch.nes", Kind::F8, 300},
    {"branch_timing_tests/3.Forward_Branch.nes", Kind::F8, 300},
    {"cpu_dummy_reads/cpu_dummy_reads.nes", Kind::Screen, 300},
    {"cpu_dummy_writes/cpu_dummy_writes_oam.nes", Kind::Status, 600},
    {"cpu_dummy_writes/cpu_dummy_writes_ppumem.nes", Kind::Status, 600},
    {"cpu_exec_space/test_cpu_exec_space_ppuio.nes", Kind::Status, 600},
    {"cpu_exec_space/test_cpu_exec_space_apu.nes", Kind::Status, 600},
    {"cpu_interrupts_v2/rom_singles/1-cli_latency.nes", Kind::Status, 600},
    {"cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes", Kind::Status, 600},
    {"cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes", Kind::Status, 600},
    {"cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes", Kind::Status, 600},
    {"cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes", Kind::Status, 600},
    {"cpu_reset/ram_after_reset.nes", Kind::Status, 600},
    {"cpu_reset/registers.nes", Kind::Status, 600},
    // Picture.
    {"blargg_ppu_tests_2005.09.15b/palette_ram.nes", Kind::Screen, 120, true, "", "$01"},
    {"blargg_ppu_tests_2005.09.15b/power_up_palette.nes", Kind::Screen, 120, true, "", "$01"},
    {"blargg_ppu_tests_2005.09.15b/sprite_ram.nes", Kind::Screen, 120, true, "", "$01"},
    {"blargg_ppu_tests_2005.09.15b/vbl_clear_time.nes", Kind::Screen, 120, true, "", "$01"},
    {"blargg_ppu_tests_2005.09.15b/vram_access.nes", Kind::Screen, 120, true, "", "$01"},
    {"ppu_vbl_nmi/rom_singles/01-vbl_basics.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/04-nmi_control.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/05-nmi_timing.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/06-suppression.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes", Kind::Status, 600},
    {"ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes", Kind::Status, 600},
    {"vbl_nmi_timing/1.frame_basics.nes", Kind::F8, 300},
    {"vbl_nmi_timing/2.vbl_timing.nes", Kind::F8, 300},
    {"vbl_nmi_timing/3.even_odd_frames.nes", Kind::F8, 300},
    {"vbl_nmi_timing/4.vbl_clear_timing.nes", Kind::F8, 300},
    {"vbl_nmi_timing/5.nmi_suppression.nes", Kind::F8, 300},
    {"vbl_nmi_timing/6.nmi_disable.nes", Kind::F8, 300},
    {"vbl_nmi_timing/7.nmi_timing.nes", Kind::F8, 300},
    {"sprite_hit_tests_2005.10.05/01.basics.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/02.alignment.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/03.corners.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/04.flip.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/05.left_clip.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/06.right_edge.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/07.screen_bottom.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/08.double_height.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/09.timing_basics.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/10.timing_order.nes", Kind::F8, 120},
    {"sprite_hit_tests_2005.10.05/11.edge_timing.nes", Kind::F8, 120},
    {"sprite_overflow_tests/1.Basics.nes", Kind::F8, 120},
    {"sprite_overflow_tests/2.Details.nes", Kind::F8, 120},
    {"sprite_overflow_tests/3.Timing.nes", Kind::F8, 120},
    {"sprite_overflow_tests/4.Obscure.nes", Kind::F8, 120},
    {"sprite_overflow_tests/5.Emulator.nes", Kind::F8, 120},
    {"oam_read/oam_read.nes", Kind::Status, 600},
    {"oam_stress/oam_stress.nes", Kind::Status, 1800},
    {"ppu_open_bus/ppu_open_bus.nes", Kind::Status, 600},
    {"ppu_read_buffer/test_ppu_read_buffer.nes", Kind::Status, 1800},
    // Sound and DMA.
    {"apu_test/rom_singles/1-len_ctr.nes", Kind::Status, 600},
    {"apu_test/rom_singles/2-len_table.nes", Kind::Status, 600},
    {"apu_test/rom_singles/3-irq_flag.nes", Kind::Status, 600},
    {"apu_test/rom_singles/4-jitter.nes", Kind::Status, 600},
    {"apu_test/rom_singles/5-len_timing.nes", Kind::Status, 600},
    {"apu_test/rom_singles/6-irq_flag_timing.nes", Kind::Status, 600},
    {"apu_test/rom_singles/7-dmc_basics.nes", Kind::Status, 600},
    {"apu_test/rom_singles/8-dmc_rates.nes", Kind::Status, 600},
    {"apu_reset/4015_cleared.nes", Kind::Status, 600},
    {"apu_reset/4017_timing.nes", Kind::Status, 600},
    {"apu_reset/4017_written.nes", Kind::Status, 600},
    {"apu_reset/irq_flag_cleared.nes", Kind::Status, 600},
    {"apu_reset/len_ctrs_enabled.nes", Kind::Status, 600},
    {"apu_reset/works_immediately.nes", Kind::Status, 600},
    {"blargg_apu_2005.07.30/01.len_ctr.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/02.len_table.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/03.irq_flag.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/04.clock_jitter.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/05.len_timing_mode0.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/06.len_timing_mode1.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/07.irq_flag_timing.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/08.irq_timing.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/09.reset_timing.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/10.len_halt_timing.nes", Kind::Screen, 300, true, "", "$01"},
    {"blargg_apu_2005.07.30/11.len_reload_timing.nes", Kind::Screen, 300, true, "", "$01"},
    // These print a CRC of what they saw; which of those listed depends on how the
    // processor and the PPU line up at power on.
    // The same on a PAL console.
    {"pal_apu_tests/01.len_ctr.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/02.len_table.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/03.irq_flag.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/04.clock_jitter.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/05.len_timing_mode0.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/06.len_timing_mode1.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/07.irq_flag_timing.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/08.irq_timing.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/10.len_halt_timing.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"pal_apu_tests/11.len_reload_timing.nes", Kind::F8, 300, true, "", "Passed", -1, true},
    {"dmc_dma_during_read4/dma_2007_read.nes", Kind::Screen, 120, true, "", "159A7A8F|5E3DF9C4"},
    {"dmc_dma_during_read4/dma_2007_write.nes", Kind::Screen, 120},
    {"dmc_dma_during_read4/dma_4016_read.nes", Kind::Screen, 120},
    {"dmc_dma_during_read4/double_2007_read.nes", Kind::Screen, 120, true, "", "85CFD627|F018C287|440EF923|E52F41A5"},
    {"dmc_dma_during_read4/read_write_2007.nes", Kind::Screen, 120},
    {"sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes", Kind::Status, 600},
    {"sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes", Kind::Status, 600},
    // Mappers.
    {"mmc3_test_2/rom_singles/1-clocking.nes", Kind::Status, 600},
    {"mmc3_test_2/rom_singles/2-details.nes", Kind::Status, 600},
    {"mmc3_test_2/rom_singles/3-A12_clocking.nes", Kind::Status, 600},
    {"mmc3_test_2/rom_singles/4-scanline_timing.nes", Kind::Status, 600},
    {"mmc3_test_2/rom_singles/5-MMC3.nes", Kind::Status, 600},
    // The older MMC3A and the MMC6 behave otherwise: their headers made NES 2.0 with the
    // submapper (4 and 1) that tells those chips apart.
    {"mmc3_test_2/rom_singles/6-MMC3_alt.nes", Kind::Status, 600, true, "", "Passed", 4},
    {"mmc3_test/1-clocking.nes", Kind::Status, 600},
    {"mmc3_test/2-details.nes", Kind::Status, 600},
    {"mmc3_test/3-A12_clocking.nes", Kind::Status, 600},
    {"mmc3_test/4-scanline_timing.nes", Kind::Status, 600},
    {"mmc3_test/5-MMC3.nes", Kind::Status, 600},
    {"mmc3_test/6-MMC6.nes", Kind::Status, 600, true, "", "Passed", 1},
    {"mmc3_irq_tests/1.Clocking.nes", Kind::F8, 200},
    {"mmc3_irq_tests/2.Details.nes", Kind::F8, 200},
    {"mmc3_irq_tests/3.A12_clocking.nes", Kind::F8, 200},
    {"mmc3_irq_tests/4.Scanline_timing.nes", Kind::F8, 200},
    {"mmc3_irq_tests/5.MMC3_rev_A.nes", Kind::F8, 200, true, "", "Passed", 4},
    {"mmc3_irq_tests/6.MMC3_rev_B.nes", Kind::F8, 200},
};

struct Result {
    bool pass = false;
    std::string detail;
};

plumnin::PadState padFrom(const std::string& buttons) {
    plumnin::PadState p;
    for (char c : buttons) {
        if (c == 'A') p.a = true;
        if (c == 'B') p.b = true;
        if (c == 'S') p.start = true;
        if (c == 's') p.select = true;
    }
    return p;
}

std::unique_ptr<Nes> load(const std::string& path, std::string* error, int submapper = -1) {
    auto nes = std::make_unique<Nes>();
    if (submapper < 0) {
        if (!nes->loadRom(path, error)) return nullptr;
    } else {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            *error = "cannot open";
            return nullptr;
        }
        std::vector<uint8_t> rom;
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) rom.insert(rom.end(), buf, buf + n);
        std::fclose(f);
        if (rom.size() < 16) return nullptr;
        rom[7] = static_cast<uint8_t>((rom[7] & 0xF3) | 0x08);
        rom[8] = static_cast<uint8_t>(submapper << 4);
        if (!nes->loadRomData(std::move(rom), error)) return nullptr;
    }
    nes->setSampleRate(44100);
    return nes;
}

bool hasSignature(const Nes& nes) {
    return nes.peek(0x6001) == 0xDE && nes.peek(0x6002) == 0xB0 && nes.peek(0x6003) == 0x61;
}

std::string statusText(const Nes& nes) {
    std::string s;
    for (uint16_t a = 0x6004; a < 0x7000; ++a) {
        const uint8_t c = nes.peek(a);
        if (!c) break;
        s += static_cast<char>(c);
    }
    return s;
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.size() > 160 ? s.substr(0, 160) + "..." : s;
}

// Runs a test as its kind says, up to its frames (status tests may end sooner).
Result runTest(Nes& nes, const Test& t) {
    nes.setPads(padFrom(t.pad), {});
    int resetAt = -1;
    bool afterReset = false;  // the 0x81 still there is the old request until the test clears it
    for (int f = 0; f < t.frames; ++f) {
        nes.runFrame();
        if (t.kind != Kind::Status || !hasSignature(nes)) continue;
        const uint8_t status = nes.peek(0x6000);
        if (afterReset) {
            if (status != 0x81) afterReset = false;
            continue;
        }
        if (status == 0x81) {
            // Asks for the reset button, at least 100 ms later.
            if (resetAt < 0) resetAt = f + 10;
            if (f == resetAt) {
                nes.reset();
                resetAt = -1;
                afterReset = true;
            }
            continue;
        }
        if (status < 0x80) {
            // A few more frames so the text is complete.
            for (int i = 0; i < 5; ++i) nes.runFrame();
            break;
        }
    }
    Result r;
    switch (t.kind) {
        case Kind::Status:
            if (!hasSignature(nes)) {
                r.detail = "no result at 6000; screen: " + oneLine(nes.screenText());
                break;
            }
            r.pass = nes.peek(0x6000) == 0;
            r.detail = "status " + std::to_string(nes.peek(0x6000)) + ": " + oneLine(statusText(nes));
            break;
        case Kind::F8:
            r.pass = nes.peek(0xF8) == 1;
            r.detail = "result " + std::to_string(nes.peek(0xF8)) + "; screen: " + oneLine(nes.screenText());
            break;
        case Kind::Screen: {
            const std::string text = nes.screenText(), words = t.words;
            size_t start = 0;
            while (!r.pass && start <= words.size()) {
                size_t end = words.find('|', start);
                if (end == std::string::npos) end = words.size();
                r.pass = text.find(words.substr(start, end - start)) != std::string::npos;
                start = end + 1;
            }
            r.detail = oneLine(text);
            break;
        }
    }
    return r;
}

bool writePng(const Nes& nes, const std::string& path) {
    const auto& f = nes.frame();
    std::vector<unsigned char> rgb(static_cast<size_t>(f.width * f.height * 3));
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            const uint32_t c = f.pixel(x, y);
            unsigned char* p = &rgb[static_cast<size_t>((y * f.width + x) * 3)];
            p[0] = static_cast<unsigned char>(c >> 16), p[1] = static_cast<unsigned char>(c >> 8),
            p[2] = static_cast<unsigned char>(c);
        }
    return stbi_write_png(path.c_str(), f.width, f.height, 3, rgb.data(), f.width * 3) != 0;
}

int runOne(int argc, char** argv) {
    std::string rom, png, pad;
    int frames = 600;
    bool pal = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--pad" && i + 1 < argc) pad = argv[++i];
        else if (a == "--pal") pal = true;
        else rom = a;
    }
    std::string error;
    auto nes = load(rom, &error);
    if (!nes) {
        std::fprintf(stderr, "%s: %s\n", rom.c_str(), error.c_str());
        return 2;
    }
    if (pal) nes->setRegion(plumnin::Region::EuropePal);
    Test t{rom.c_str(), Kind::Status, frames};
    t.pad = pad.c_str();
    const Result r = runTest(*nes, t);
    std::printf("6000: %s\nF8: %d\nscreen:\n%s\n", r.detail.c_str(), nes->peek(0xF8), nes->screenText().c_str());
    if (!png.empty() && !writePng(*nes, png)) std::fprintf(stderr, "could not write %s\n", png.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--run") == 0) return runOne(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: nes-test-roms ROOT [--all] | --run ROM [--frames N] [--png OUT] [--pad AB]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const bool all = argc >= 3 && std::strcmp(argv[2], "--all") == 0;
    int passed = 0, failed = 0, unexpected = 0, skipped = 0;
    for (const Test& t : kTests) {
        if (!t.expected && !all) {
            ++skipped;
            continue;
        }
        const std::string path = (root / t.path).string();
        std::string error;
        auto nes = load(path, &error, t.submapper);
        Result r;
        if (!nes) r.detail = error;
        else {
            if (t.pal) nes->setRegion(plumnin::Region::EuropePal);
            r = runTest(*nes, t);
        }
        std::printf("%s %s%s%s\n", r.pass ? "pass" : "FAIL", t.path, r.pass ? "" : "  ", r.pass ? "" : r.detail.c_str());
        if (r.pass) {
            ++passed;
        } else {
            ++failed;
            if (t.expected) ++unexpected;
        }
    }
    std::printf("\n%d passed, %d failed (%d expected to pass), %d known failures not run\n", passed, failed, unexpected,
                skipped);
    return unexpected ? 1 : 0;
}
