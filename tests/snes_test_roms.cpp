// Runs the SNES test ROMs of krom (Peter Lemon, https://github.com/PeterLemon/SNES) on
// plumnin's SNES and says which pass. They are not part of plumnin: clone that
// repository and give its folder.
//
//   snes-test-roms ROOT                 the suite below; exit code 1 if one fails
//   snes-test-roms --run ROM [--frames N] [--png OUT]   one ROM, to look at it
//
// A test passes in one of two ways, depending on what it does:
//   screen   it prints its results: every line says PASS (or PASSED), none FAIL, at
//            any moment while it runs (the processor tests go through several screens)
//   notes    a tune: the main note of the sound at given times, in Hz (within 3%)

#include <cmath>
#include <complex>
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

#include "snes/snes.h"

using plumnin::Snes;

namespace {

enum class Kind { Screen, Notes };

struct Test {
    const char* path;
    Kind kind;
    int frames;
    std::vector<std::pair<double, double>> notes = {};  // for Notes: (second, Hz)
};

const std::vector<Test> kTests = {
    // The processor: each instruction in each addressing mode, binary and decimal.
    {"CPUTest/CPU/ADC/CPUADC.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/AND/CPUAND.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/ASL/CPUASL.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/BIT/CPUBIT.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/BRA/CPUBRA.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/CMP/CPUCMP.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/DEC/CPUDEC.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/EOR/CPUEOR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/INC/CPUINC.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/JMP/CPUJMP.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/LDR/CPULDR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/LSR/CPULSR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/MOV/CPUMOV.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/MSC/CPUMSC.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/ORA/CPUORA.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/PHL/CPUPHL.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/PSR/CPUPSR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/RET/CPURET.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/ROL/CPUROL.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/ROR/CPUROR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/SBC/CPUSBC.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/STR/CPUSTR.sfc", Kind::Screen, 120},
    {"CPUTest/CPU/TRN/CPUTRN.sfc", Kind::Screen, 120},
    // The cartridge's mappings and the WRAM's banks.
    {"BANK/LoROMSlowROM/BANKLoROMSlowROM.sfc", Kind::Screen, 30},
    {"BANK/LoROMFastROM/BANKLoROMFastROM.sfc", Kind::Screen, 30},
    {"BANK/HiROMSlowROM/BANKHiROMSlowROM.sfc", Kind::Screen, 30},
    {"BANK/HiROMFastROM/BANKHiROMFastROM.sfc", Kind::Screen, 30},
    {"BANK/WRAM/BANKWRAM.sfc", Kind::Screen, 30},
    // The sound module: Twinkle Twinkle Little Star's first notes (C C G G A A G F F E E D)
    // played from a BRR sample, which checks the IPL upload, the DSP's pitch and envelopes.
    {"SPC700/Twinkle/Twinkle.sfc",
     Kind::Notes,
     270,
     {{0.5, 523.3}, {1.0, 784.0}, {1.5, 880.0}, {2.0, 784.0}, {2.5, 698.5}, {3.5, 659.3}, {4.0, 587.3}}},
};

struct Result {
    bool pass = false;
    std::string detail;
};

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n') c = '|';
    return s.size() > 200 ? s.substr(0, 200) + "..." : s;
}

// The strongest frequency of the left channel around `second`.
double mainFrequency(const std::vector<int16_t>& stereo, double rate, double second) {
    const size_t n = 8192;
    const size_t start = static_cast<size_t>(second * rate);
    if ((start + n) * 2 > stereo.size()) return 0;
    std::vector<std::complex<double>> x(n);
    const double pi = 3.14159265358979323846;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * pi * static_cast<double>(i) / (n - 1));
        x[i] = stereo[(start + i) * 2] * w;
    }
    // An iterative radix-2 FFT.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> step = std::polar(1.0, -2 * pi / static_cast<double>(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1;
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    size_t best = 20;
    for (size_t k = 20; k < n / 2; ++k)
        if (std::abs(x[k]) > std::abs(x[best])) best = k;
    return static_cast<double>(best) * rate / static_cast<double>(n);
}

Result runTest(Snes& snes, const Test& t) {
    Result r;
    if (t.kind == Kind::Screen) {
        std::string text, failure;
        for (int f = 0; f < t.frames; ++f) {
            snes.runFrame();
            text = snes.screenText();
            if (failure.empty() && text.find("FAIL") != std::string::npos) failure = text;
        }
        size_t passes = 0;
        for (size_t p = 0; (p = text.find("PASS", p)) != std::string::npos; p += 4) ++passes;
        r.pass = failure.empty() && passes > 0;
        r.detail = oneLine(failure.empty() ? text : failure);
        return r;
    }
    const double rate = 44100;
    snes.setSampleRate(rate);
    std::vector<int16_t> sound;
    for (int f = 0; f < t.frames; ++f) {
        snes.runFrame();
        const auto& s = snes.audio().samples();
        sound.insert(sound.end(), s.begin(), s.end());
        snes.audio().clearSamples();
    }
    r.pass = true;
    for (const auto& [second, hz] : t.notes) {
        const double got = mainFrequency(sound, rate, second);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1fs: %.0f Hz (want %.0f) ", second, got, hz);
        r.detail += buf;
        if (std::fabs(got - hz) > hz * 0.03) r.pass = false;
    }
    return r;
}

bool writePng(const Snes& snes, const std::string& path) {
    const auto& f = snes.frame();
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
    std::string rom, png;
    int frames = 300;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else rom = a;
    }
    Snes snes;
    std::string error;
    if (!snes.loadRom(rom, &error)) {
        std::fprintf(stderr, "%s: %s\n", rom.c_str(), error.c_str());
        return 2;
    }
    for (int f = 0; f < frames; ++f) snes.runFrame();
    std::printf("title: %s\nscreen:\n%s\n", snes.title().c_str(), snes.screenText().c_str());
    if (!png.empty() && !writePng(snes, png)) std::fprintf(stderr, "could not write %s\n", png.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--run") == 0) return runOne(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: snes-test-roms ROOT | --run ROM [--frames N] [--png OUT]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    int passed = 0, failed = 0;
    for (const Test& t : kTests) {
        Snes snes;
        std::string error;
        Result r;
        if (!snes.loadRom((root / t.path).string(), &error)) r.detail = error;
        else r = runTest(snes, t);
        std::printf("%s %s%s%s\n", r.pass ? "pass" : "FAIL", t.path, r.pass ? "" : "  ", r.pass ? "" : r.detail.c_str());
        ++(r.pass ? passed : failed);
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
