#include "system.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "i18n.h"
#include "miniz.h"

namespace plumnin {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

bool startsWith(const std::vector<uint8_t>& d, size_t at, const char* text, size_t n) {
    return d.size() >= at + n && std::equal(text, text + n, d.begin() + static_cast<long>(at),
                                            [](char t, uint8_t b) { return static_cast<uint8_t>(t) == b; });
}

// The start of the Nintendo logo every Game Boy cartridge carries at 0x104: the boot
// ROM refuses to start one without it.
const char kGbLogo[] = "\xCE\xED\x66\x66\xCC\x0D\x00\x0B";

// A Game Boy header's CGB flag (0x143): 0x80 works on both, 0xC0 only on the Color.
bool colorOnly(const std::vector<uint8_t>& d) { return d.size() > 0x143 && (d[0x143] & 0x80) != 0; }

bool readWhole(const std::string& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    if (fs::is_directory(fs::u8path(path), ec)) return false;  // a folder named like a game
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The first cartridge in a zip archive, with its name. False if there is none.
bool unzipRom(const std::vector<uint8_t>& data, std::vector<uint8_t>& rom, std::string* name, bool* damaged) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        *damaged = true;
        return false;
    }
    bool found = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !found; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory || !hasRomExtension(st.m_filename)) continue;
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!bytes) continue;
        rom.assign(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + size);
        mz_free(bytes);
        if (name) *name = st.m_filename;
        found = true;
    }
    mz_zip_reader_end(&zip);
    return found;
}

}  // namespace

const char* systemName(ConsoleSystem s) {
    switch (s) {
        case ConsoleSystem::GameBoyColor: return "Game Boy Color";
        case ConsoleSystem::Nes: return "NES";
        case ConsoleSystem::Snes: return "Super Nintendo";
        default: return "Game Boy";
    }
}

const char* systemKey(ConsoleSystem s) {
    switch (s) {
        case ConsoleSystem::GameBoyColor: return "GameBoyColor";
        case ConsoleSystem::Nes: return "NES";
        case ConsoleSystem::Snes: return "SNES";
        default: return "GameBoy";
    }
}

bool hasRomExtension(const std::string& path) {
    const std::string e = extensionOf(path);
    return e == ".gb" || e == ".gbc" || e == ".cgb" || e == ".nes" || e == ".sfc" || e == ".smc";
}

bool isGameFile(const std::string& path) { return hasRomExtension(path) || extensionOf(path) == ".zip"; }

ConsoleSystem detectSystemFromData(const std::vector<uint8_t>& d) {
    if (startsWith(d, 0, "NES\x1A", 4)) return ConsoleSystem::Nes;
    if (startsWith(d, 0x104, kGbLogo, sizeof kGbLogo - 1))
        return colorOnly(d) ? ConsoleSystem::GameBoyColor : ConsoleSystem::GameBoy;
    return ConsoleSystem::Snes;
}

ConsoleSystem detectSystem(const std::string& path) {
    const std::string e = extensionOf(path);
    if (e == ".nes") return ConsoleSystem::Nes;
    if (e == ".sfc" || e == ".smc") return ConsoleSystem::Snes;
    if (e == ".gbc" || e == ".cgb") return ConsoleSystem::GameBoyColor;
    // A .gb can still be a Color game, and a .zip anything: look inside.
    std::vector<uint8_t> rom;
    std::string name, error;
    if (!readRomFile(path, rom, &name, &error)) return e == ".gb" ? ConsoleSystem::GameBoy : ConsoleSystem::Snes;
    if (e == ".zip") {
        const std::string inner = extensionOf(name);
        if (inner == ".nes") return ConsoleSystem::Nes;
        if (inner == ".sfc" || inner == ".smc") return ConsoleSystem::Snes;
        if (inner == ".gbc" || inner == ".cgb") return ConsoleSystem::GameBoyColor;
    }
    return detectSystemFromData(rom);
}

bool readRomFile(const std::string& path, std::vector<uint8_t>& rom, std::string* name, std::string* error) {
    std::vector<uint8_t> data;
    if (!readWhole(path, data)) {
        if (error) *error = trf("no se pudo leer %s", path.c_str());
        return false;
    }
    if (extensionOf(path) != ".zip") {
        rom = std::move(data);
        if (name) *name = fs::u8path(path).filename().u8string();
        if (rom.empty() && error) *error = trf("el archivo está vacío: %s", path.c_str());
        return !rom.empty();
    }
    bool damaged = false;
    if (unzipRom(data, rom, name, &damaged)) return true;
    if (error)
        *error = damaged ? trf("el zip está dañado: %s", path.c_str())
                         : trf("el zip no contiene ninguna ROM (.gb, .gbc, .nes, .sfc o .smc): %s", path.c_str());
    return false;
}

}  // namespace plumnin
