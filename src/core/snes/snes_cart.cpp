#include "snes_cart.h"

#include "../i18n.h"

namespace plumnin::snes {

namespace {

// How much a candidate header looks like the real one.
int headerScore(const std::vector<uint8_t>& rom, size_t base, Mapping mapping) {
    if (rom.size() < base + 0x40) return -1000;
    int score = 0;
    const uint8_t map = rom[base + 0x15];
    const uint16_t complement = static_cast<uint16_t>(rom[base + 0x1C] | rom[base + 0x1D] << 8);
    const uint16_t checksum = static_cast<uint16_t>(rom[base + 0x1E] | rom[base + 0x1F] << 8);
    const uint16_t reset = static_cast<uint16_t>(rom[base + 0x3C] | rom[base + 0x3D] << 8);
    if ((complement ^ checksum) == 0xFFFF) score += 8;
    const int kind = map & 0xEF;  // without the FastROM bit
    if (mapping == Mapping::LoRom && (kind == 0x20 || kind == 0x22 || kind == 0x23)) score += 4;
    if (mapping == Mapping::HiRom && kind == 0x21) score += 4;
    if (mapping == Mapping::ExHiRom && kind == 0x25) score += 4;
    if (reset >= 0x8000) score += 2;
    else score -= 8;
    bool printable = true;
    for (size_t i = 0; i < 21; ++i)
        if (rom[base + i] < 0x20 || rom[base + i] > 0x7E) printable = false;
    if (printable) score += 2;
    if (rom[base + 0x18] <= 0x08) ++score;
    if (rom[base + 0x19] <= 0x14) ++score;
    // The first instruction of the reset code: most games start with one of these.
    size_t at = mapping == Mapping::LoRom ? (reset & 0x7FFF) : mapping == Mapping::HiRom ? reset : 0x400000u + reset;
    if (base == 0x7FC0 || base == 0xFFC0 || base == 0x40FFC0) {
        if (at < rom.size()) {
            switch (rom[at]) {
                case 0x78:  // sei
                case 0x18:  // clc
                case 0x38:  // sec
                case 0x9C:  // stz
                case 0x4C:  // jmp
                case 0x5C:  // jml
                case 0xC2:  // rep
                case 0xE2:  // sep
                case 0xAD:  // lda abs
                case 0xA9:  // lda #
                case 0xA2:  // ldx #
                case 0x20:  // jsr
                case 0x22: score += 3; break;  // jsl
                case 0x00:
                case 0xFF: score -= 4; break;
                default: break;
            }
        }
    }
    return score;
}

// Which chip: the header's chipset byte, its subtype (FFBF) for the custom ones, and the
// title where one kind of chip had several programs.
Chip chipOf(const CartInfo& info, uint8_t subtype) {
    const std::string& t = info.title;
    switch (info.chipset >> 4) {
        case 0x0:
            if (t.find("DUNGEON MASTER") != std::string::npos) return Chip::Dsp2;
            if (t.find("TOP GEAR 3000") != std::string::npos || t.find("PLANETS CHAMP") != std::string::npos)
                return Chip::Dsp4;
            if (info.rawTitle.compare(0, 2, "SD") == 0 && info.rawTitle.find("GX") != std::string::npos) return Chip::Dsp3;
            return Chip::Dsp1;
        case 0x1: return Chip::SuperFx;
        case 0x2: return Chip::Obc1;
        case 0x3: return Chip::Sa1;
        case 0x4: return Chip::Sdd1;
        case 0x5: return Chip::None;  // S-RTC: only a clock, the game runs without it
        case 0xE: return Chip::SuperGameBoy;
        case 0xF:
            switch (subtype) {
                case 0x01: return t.find("SHOUGI") != std::string::npos ? Chip::St011 : Chip::St010;
                case 0x02: return Chip::St018;
                case 0x10: return Chip::Cx4;
                default: return Chip::Spc7110;
            }
        default: return Chip::None;
    }
}

}  // namespace

const char* chipName(Chip chip) {
    switch (chip) {
        case Chip::None: return "";
        case Chip::Dsp1: return "DSP-1";
        case Chip::Dsp2: return "DSP-2";
        case Chip::Dsp3: return "DSP-3";
        case Chip::Dsp4: return "DSP-4";
        case Chip::St010: return "ST010";
        case Chip::St011: return "ST011";
        case Chip::St018: return "ST018";
        case Chip::SuperFx: return "Super FX";
        case Chip::Sa1: return "SA-1";
        case Chip::Sdd1: return "S-DD1";
        case Chip::Obc1: return "OBC1";
        case Chip::Cx4: return "Cx4";
        case Chip::Spc7110: return "SPC7110";
        case Chip::SuperGameBoy: return "Super Game Boy";
    }
    return "";
}

uint32_t Cart::mirror(uint32_t offset, uint32_t size) const {
    // A size that is not a power of two repeats its last part, as the chips are wired.
    if (!size) return 0;
    uint32_t base = 0, mask = 1u << 24;
    while (offset >= size) {
        while (!(offset & mask)) mask >>= 1;
        offset -= mask;
        if (size > mask) {
            size -= mask;
            base += mask;
        }
        mask >>= 1;
    }
    return base + offset;
}

bool Cart::load(const std::vector<uint8_t>& file, std::string* error) {
    std::vector<uint8_t> rom = file;
    // A copier's 512-byte header in front.
    if (rom.size() % 1024 == 512) rom.erase(rom.begin(), rom.begin() + 512);
    if (rom.size() < 0x8000) {
        if (error) *error = tr("el archivo es demasiado pequeño para ser un juego de Super Nintendo");
        return false;
    }
    struct Candidate {
        size_t base;
        Mapping mapping;
    };
    const Candidate candidates[] = {{0x7FC0, Mapping::LoRom}, {0xFFC0, Mapping::HiRom}, {0x40FFC0, Mapping::ExHiRom}};
    int best = -1000;
    Candidate chosen = candidates[0];
    for (const Candidate& c : candidates) {
        const int s = headerScore(rom, c.base, c.mapping);
        if (s > best) best = s, chosen = c;
    }
    CartInfo info;
    const size_t h = chosen.base;
    info.mapping = chosen.mapping;
    for (size_t i = 0; i < 21; ++i) {
        const char c = static_cast<char>(rom[h + i]);
        info.rawTitle += c;
        info.title += c >= 0x20 && c < 0x7F ? c : ' ';
    }
    while (!info.title.empty() && info.title.back() == ' ') info.title.pop_back();
    info.fastRom = rom[h + 0x15] & 0x10;
    info.chipset = rom[h + 0x16];
    const uint8_t ramSize = rom[h + 0x18];
    info.ramSize = ramSize && ramSize <= 0x08 ? 1024u << ramSize : 0;
    const uint8_t region = rom[h + 0x19];
    info.pal = (region >= 0x02 && region <= 0x0C) || region == 0x11;
    const int parts = info.chipset & 0x0F;
    info.battery = parts == 0x02 || parts == 0x05 || parts == 0x06 || parts == 0x09 || parts == 0x0A;
    if (parts >= 0x03) info.chip = chipOf(info, rom[h - 1]);
    info.coprocessor = chipName(info.chip);
    // The Super FX's RAM: its size is in the extended header (FFBD) when there is one
    // (FFDA is 33), else 32 KB, as on Star Fox.
    if (info.chip == Chip::SuperFx) {
        const uint8_t ext = rom[h - 3];
        info.ramSize = rom[h + 0x1A] == 0x33 && ext && ext <= 0x08 ? 1024u << ext : 0x8000;
    }
    if (!info.ramSize && info.battery) info.ramSize = 0x2000;
    info_ = info;
    rom_ = std::move(rom);
    // The Super FX's RAM is where it draws: it starts blank, as other emulators have it
    // (programs count on that).
    ram_.assign(info_.ramSize, info_.chip == Chip::SuperFx ? 0x00 : 0xFF);
    ramChanged_ = false;
    return true;
}

int64_t Cart::romOffset(uint32_t address) const {
    const uint32_t bank = address >> 16, a = address & 0xFFFF;
    const uint32_t size = static_cast<uint32_t>(rom_.size());
    switch (info_.mapping) {
        case Mapping::LoRom:
            if ((bank & 0x7F) < 0x40 && a < 0x8000) return -1;
            if ((bank & 0x7F) >= 0x7E && bank < 0x80) return -1;
            return mirror(((bank & 0x7F) << 15) | (a & 0x7FFF), size);
        case Mapping::HiRom:
            if ((bank & 0x7F) < 0x40 && a < 0x8000) return -1;
            if ((bank & 0x7F) >= 0x7E && bank < 0x80) return -1;
            return mirror(((bank & 0x3F) << 16) | a, size);
        case Mapping::ExHiRom: {
            if ((bank & 0x7F) < 0x40 && a < 0x8000) return -1;
            if ((bank & 0x7F) >= 0x7E && bank < 0x80) return -1;
            const uint32_t high = bank & 0x80 ? 0 : 0x400000;
            return mirror(high | ((bank & 0x3F) << 16) | a, size);
        }
    }
    return -1;
}

int64_t Cart::ramOffset(uint32_t address) const {
    if (ram_.empty()) return -1;
    const uint32_t bank = address >> 16, a = address & 0xFFFF;
    const uint32_t size = static_cast<uint32_t>(ram_.size());
    if (info_.mapping == Mapping::LoRom) {
        // 70-7D and F0-FF, below 8000.
        if ((bank & 0x7F) >= 0x70 && (bank & 0x7F) < 0x7E && a < 0x8000) return mirror(((bank & 0x0F) << 15) | a, size);
        return -1;
    }
    // HiROM and ExHiROM: 6000-7FFF of banks 20-3F and A0-BF.
    if ((bank & 0x7F) >= 0x20 && (bank & 0x7F) < 0x40 && a >= 0x6000 && a < 0x8000)
        return mirror(((bank & 0x1F) << 13) | (a - 0x6000), size);
    return -1;
}

bool Cart::read(uint32_t address, uint8_t& value) const {
    const int64_t r = ramOffset(address);
    if (r >= 0) {
        value = ram_[static_cast<size_t>(r)];
        return true;
    }
    const int64_t o = romOffset(address);
    if (o < 0 || static_cast<size_t>(o) >= rom_.size()) return false;
    value = rom_[static_cast<size_t>(o)];
    return true;
}

bool Cart::write(uint32_t address, uint8_t value) {
    const int64_t r = ramOffset(address);
    if (r < 0) return false;
    uint8_t& b = ram_[static_cast<size_t>(r)];
    if (b != value) ramChanged_ = true;
    b = value;
    return true;
}

}  // namespace plumnin::snes
