#include "gb_cart.h"

#include <algorithm>
#include <ctime>

#include "../i18n.h"

namespace plumnin::gb {

bool Cartridge::load(const std::vector<uint8_t>& rom, std::string* error) {
    if (rom.size() < 0x150) {
        if (error) *error = tr("la ROM es demasiado pequeña para ser de Game Boy");
        return false;
    }
    const uint8_t type = rom[0x147];
    battery_ = hasClock_ = rumble_ = false;
    switch (type) {
        case 0x00: mapper_ = Mapper::None; break;
        case 0x08: mapper_ = Mapper::None; break;
        case 0x09: mapper_ = Mapper::None, battery_ = true; break;
        case 0x01:
        case 0x02: mapper_ = Mapper::Mbc1; break;
        case 0x03: mapper_ = Mapper::Mbc1, battery_ = true; break;
        case 0x05: mapper_ = Mapper::Mbc2; break;
        case 0x06: mapper_ = Mapper::Mbc2, battery_ = true; break;
        case 0x0F:
        case 0x10: mapper_ = Mapper::Mbc3, battery_ = hasClock_ = true; break;
        case 0x11:
        case 0x12: mapper_ = Mapper::Mbc3; break;
        case 0x13: mapper_ = Mapper::Mbc3, battery_ = true; break;
        case 0x19:
        case 0x1A: mapper_ = Mapper::Mbc5; break;
        case 0x1B: mapper_ = Mapper::Mbc5, battery_ = true; break;
        case 0x1C:
        case 0x1D: mapper_ = Mapper::Mbc5, rumble_ = true; break;
        case 0x1E: mapper_ = Mapper::Mbc5, rumble_ = battery_ = true; break;
        default:
            if (error) *error = trf("el cartucho usa un chip que plumnin no emula (tipo %02X)", type);
            return false;
    }
    // The ROM grows to a power of two of banks, the way the chip's address lines see it.
    size_t size = 0x8000;
    while (size < rom.size()) size *= 2;
    rom_ = rom;
    rom_.resize(size, 0xFF);
    romMask_ = size - 1;
    mbc30_ = mapper_ == Mapper::Mbc3 && size > 0x200000;

    size_t ramSize = 0;
    switch (rom[0x149]) {
        case 1: ramSize = 0x800; break;
        case 2: ramSize = 0x2000; break;
        case 3: ramSize = 0x8000; break;
        case 4: ramSize = 0x20000; break;
        case 5: ramSize = 0x10000; break;
        default: break;
    }
    if (mapper_ == Mapper::Mbc2) ramSize = 512;
    // A ROM-only cartridge with RAM in its type but none in its size: 8 KB, as emulators do.
    if ((type == 0x08 || type == 0x09) && ramSize == 0) ramSize = 0x2000;
    ram_.assign(ramSize, 0xFF);
    std::fill(std::begin(rtc_), std::end(rtc_), 0);
    std::fill(std::begin(rtcLatched_), std::end(rtcLatched_), 0);
    rtcCycles_ = 0;
    changed_ = false;
    reset();
    return true;
}

void Cartridge::reset() {
    ramEnabled_ = false;
    bank1_ = 1;
    bank2_ = 0;
    mode_ = 0;
    romBank_ = 1;
    ramBank_ = 0;
    latch_ = 0xFF;
}

std::string Cartridge::title() const {
    std::string t;
    // 16 bytes on the first games, 15 or 11 when the Color's flag and maker code took the rest.
    const size_t end = colorGame() ? 0x143 : 0x144;
    for (size_t i = 0x134; i < end && rom_[i]; ++i)
        if (rom_[i] >= 0x20 && rom_[i] < 0x7F) t += static_cast<char>(rom_[i]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

size_t Cartridge::romOffset0() const {
    if (mapper_ == Mapper::Mbc1 && mode_) return (static_cast<size_t>(bank2_) << 19) & romMask_;
    return 0;
}

size_t Cartridge::romOffsetN() const {
    size_t bank = 1;
    switch (mapper_) {
        case Mapper::None: bank = 1; break;
        case Mapper::Mbc1: bank = static_cast<size_t>(bank2_) << 5 | bank1_; break;
        default: bank = romBank_; break;
    }
    return (bank << 14) & romMask_;
}

size_t Cartridge::ramOffset() const {
    if (ram_.size() <= 0x2000) return 0;
    size_t bank = 0;
    if (mapper_ == Mapper::Mbc1) bank = mode_ ? bank2_ : 0;
    else if (mapper_ == Mapper::Mbc3) bank = ramBank_ & 7;
    else if (mapper_ == Mapper::Mbc5) bank = ramBank_ & (rumble_ ? 0x07 : 0x0F);
    return (bank << 13) & (ram_.size() - 1);
}

uint8_t Cartridge::readRom(uint16_t address) const {
    if (address < 0x4000) return rom_[romOffset0() | address];
    return rom_[romOffsetN() | (address & 0x3FFF)];
}

uint8_t Cartridge::readRam(uint16_t address) const {
    if (mapper_ == Mapper::Mbc2) {
        if (!ramEnabled_) return 0xFF;
        return ram_[address & 0x1FF] | 0xF0;
    }
    if (mapper_ == Mapper::Mbc3 && ramEnabled_ && ramBank_ >= 8) {
        if (!hasClock_ || ramBank_ > 0x0C) return 0xFF;
        static const uint8_t masks[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
        return rtcLatched_[ramBank_ - 8] & masks[ramBank_ - 8];
    }
    if (ram_.empty() || (mapper_ != Mapper::None && !ramEnabled_)) return 0xFF;
    if (mapper_ == Mapper::Mbc3 && !mbc30_ && ramBank_ > 3) return 0xFF;
    return ram_[ramOffset() | (address & (std::min<size_t>(ram_.size(), 0x2000) - 1))];
}

void Cartridge::write(uint16_t address, uint8_t value) {
    if (address >= 0xA000) {
        if (mapper_ == Mapper::Mbc2) {
            if (ramEnabled_) ram_[address & 0x1FF] = value | 0xF0, changed_ = true;
            return;
        }
        if (mapper_ == Mapper::Mbc3 && ramEnabled_ && ramBank_ >= 8) {
            if (!hasClock_ || ramBank_ > 0x0C) return;
            const int r = ramBank_ - 8;
            static const uint8_t masks[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            rtc_[r] = rtcLatched_[r] = value & masks[r];
            if (r == 0) rtcCycles_ = 0;  // writing the seconds starts a new second
            changed_ = true;
            return;
        }
        if (ram_.empty() || (mapper_ != Mapper::None && !ramEnabled_)) return;
        if (mapper_ == Mapper::Mbc3 && !mbc30_ && ramBank_ > 3) return;
        ram_[ramOffset() | (address & (std::min<size_t>(ram_.size(), 0x2000) - 1))] = value;
        changed_ = true;
        return;
    }
    switch (mapper_) {
        case Mapper::None: break;
        case Mapper::Mbc1:
            if (address < 0x2000) ramEnabled_ = (value & 0x0F) == 0x0A;
            else if (address < 0x4000) bank1_ = (value & 0x1F) ? (value & 0x1F) : 1;
            else if (address < 0x6000) bank2_ = value & 3;
            else mode_ = value & 1;
            break;
        case Mapper::Mbc2:
            if (address < 0x4000) {
                if (address & 0x100) romBank_ = (value & 0x0F) ? (value & 0x0F) : 1;
                else ramEnabled_ = (value & 0x0F) == 0x0A;
            }
            break;
        case Mapper::Mbc3:
            if (address < 0x2000) ramEnabled_ = (value & 0x0F) == 0x0A;
            else if (address < 0x4000) {
                romBank_ = value & (mbc30_ ? 0xFF : 0x7F);
                if (!romBank_) romBank_ = 1;
            } else if (address < 0x6000) ramBank_ = value & 0x0F;
            else {
                if (latch_ == 0 && value == 1) std::copy(std::begin(rtc_), std::end(rtc_), rtcLatched_);
                latch_ = value;
            }
            break;
        case Mapper::Mbc5:
            if (address < 0x2000) ramEnabled_ = (value & 0x0F) == 0x0A;
            else if (address < 0x3000) romBank_ = static_cast<uint16_t>((romBank_ & 0x100) | value);
            else if (address < 0x4000) romBank_ = static_cast<uint16_t>((romBank_ & 0xFF) | (value & 1) << 8);
            else if (address < 0x6000) ramBank_ = value & 0x0F;
            break;
    }
}

void Cartridge::secondPassed() {
    if (rtc_[4] & 0x40) return;  // halted
    rtc_[0] = (rtc_[0] + 1) & 63;
    if (rtc_[0] != 60) return;
    rtc_[0] = 0;
    rtc_[1] = (rtc_[1] + 1) & 63;
    if (rtc_[1] != 60) return;
    rtc_[1] = 0;
    rtc_[2] = (rtc_[2] + 1) & 31;
    if (rtc_[2] != 24) return;
    rtc_[2] = 0;
    int day = (rtc_[4] & 1) << 8 | rtc_[3];
    ++day;
    if (day > 511) {
        day = 0;
        rtc_[4] |= 0x80;
    }
    rtc_[3] = static_cast<uint8_t>(day);
    rtc_[4] = static_cast<uint8_t>((rtc_[4] & 0xFE) | (day >> 8));
}

std::vector<uint8_t> Cartridge::saveData() const {
    std::vector<uint8_t> out = ram_;
    if (mapper_ == Mapper::Mbc2)
        for (auto& b : out) b |= 0xF0;
    if (hasClock_) {
        auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        for (uint8_t r : rtc_) put32(r);
        for (uint8_t r : rtcLatched_) put32(r);
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        put32(static_cast<uint32_t>(now));
        put32(static_cast<uint32_t>(now >> 32));
    }
    return out;
}

void Cartridge::loadSaveData(const std::vector<uint8_t>& data) {
    std::copy_n(data.begin(), std::min(data.size(), ram_.size()), ram_.begin());
    if (!hasClock_ || data.size() < ram_.size() + 44) return;
    const uint8_t* p = data.data() + ram_.size();
    auto get32 = [&](size_t at) {
        return static_cast<uint32_t>(p[at]) | static_cast<uint32_t>(p[at + 1]) << 8 |
               static_cast<uint32_t>(p[at + 2]) << 16 | static_cast<uint32_t>(p[at + 3]) << 24;
    };
    for (size_t i = 0; i < 5; ++i) rtc_[i] = static_cast<uint8_t>(get32(i * 4));
    for (size_t i = 0; i < 5; ++i) rtcLatched_[i] = static_cast<uint8_t>(get32(20 + i * 4));
    uint64_t saved = get32(40);
    if (data.size() >= ram_.size() + 48) saved |= static_cast<uint64_t>(get32(44)) << 32;
    // The clock kept running while the game was off.
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (saved && now > saved) {
        uint64_t elapsed = now - saved;
        if (rtc_[4] & 0x40) return;
        // Out-of-range values (a game may write 63 seconds) go up one by one until valid.
        while (elapsed && (rtc_[0] > 59 || rtc_[1] > 59 || rtc_[2] > 23)) secondPassed(), --elapsed;
        uint64_t total = rtc_[0] + rtc_[1] * 60ull + rtc_[2] * 3600ull +
                         ((rtc_[4] & 1) << 8 | rtc_[3]) * 86400ull + elapsed;
        rtc_[0] = static_cast<uint8_t>(total % 60);
        rtc_[1] = static_cast<uint8_t>(total / 60 % 60);
        rtc_[2] = static_cast<uint8_t>(total / 3600 % 24);
        const uint64_t days = total / 86400;
        if (days > 511) rtc_[4] |= 0x80;
        rtc_[3] = static_cast<uint8_t>(days & 0xFF);
        rtc_[4] = static_cast<uint8_t>((rtc_[4] & 0xFE) | ((days >> 8) & 1));
    }
}

}  // namespace plumnin::gb
