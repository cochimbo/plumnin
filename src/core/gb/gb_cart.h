#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../state_io.h"

namespace plumnin::gb {

// The cartridge: its ROM, its RAM and the chip that switches their banks (the MBC),
// with the clock of the MBC3.
class Cartridge {
public:
    enum class Mapper : uint8_t { None, Mbc1, Mbc2, Mbc3, Mbc5 };

    // False, with the reason in Spanish, if the header asks for a chip plumnin lacks.
    bool load(const std::vector<uint8_t>& rom, std::string* error);
    void reset();  // the registers as at power on; RAM is kept

    uint8_t readRom(uint16_t address) const;
    uint8_t readRam(uint16_t address) const;
    void write(uint16_t address, uint8_t value);  // 0000-7FFF and A000-BFFF

    // The real time clock, run by the cartridge's own crystal: 4194304 ticks a second.
    void tickClock(int cycles) {
        if (!hasClock_) return;
        rtcCycles_ += cycles;
        if (rtcCycles_ >= 4194304) {
            rtcCycles_ -= 4194304;
            secondPassed();
        }
    }

    bool hasBattery() const { return battery_; }
    bool hasClock() const { return hasClock_; }
    bool hasRam() const { return !ram_.empty(); }
    // RAM plus, with a clock, the 48 bytes of it that VBA-M, BGB and mGBA write.
    std::vector<uint8_t> saveData() const;
    void loadSaveData(const std::vector<uint8_t>& data);
    bool ramChanged() const { return changed_; }
    void clearRamChanged() { changed_ = false; }

    Mapper mapper() const { return mapper_; }
    // The bank registers, to put back the banks some code ran with.
    struct Banks {
        bool ramEnabled = false;
        uint8_t bank1 = 1, bank2 = 0, mode = 0;
        uint16_t romBank = 1;
        uint8_t ramBank = 0;
    };
    Banks banks() const { return {ramEnabled_, bank1_, bank2_, mode_, romBank_, ramBank_}; }
    void setBanks(const Banks& b) {
        ramEnabled_ = b.ramEnabled, bank1_ = b.bank1, bank2_ = b.bank2, mode_ = b.mode, romBank_ = b.romBank,
        ramBank_ = b.ramBank;
    }
    // The ROM bank mapped at 4000-7FFF.
    size_t romBankNumber() const { return romOffsetN() / 0x4000; }
    size_t romSize() const { return rom_.size(); }
    // The banks mapped now, for the achievements' map.
    const uint8_t* romBank0() const { return &rom_[romOffset0()]; }
    const uint8_t* romBankN() const { return &rom_[romOffsetN()]; }
    uint8_t* ramBank() { return ram_.empty() ? nullptr : &ram_[ramOffset()]; }
    size_t ramBankSize() const { return ram_.empty() ? 0 : std::min<size_t>(ram_.size(), 0x2000); }
    std::vector<uint8_t>& ram() { return ram_; }
    std::string title() const;
    bool colorGame() const { return rom_.size() > 0x143 && (rom_[0x143] & 0x80); }
    // Rumble cartridges move a motor with bit 3 of the RAM bank.
    bool rumble() const { return rumble_; }

    template <class Ar>
    void state(Ar& ar) {
        ar(ram_, ramEnabled_, bank1_, bank2_, mode_, romBank_, ramBank_, latch_, rtc_, rtcLatched_, rtcCycles_);
    }

private:
    size_t romOffset0() const;
    size_t romOffsetN() const;
    size_t ramOffset() const;
    void secondPassed();

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_;
    Mapper mapper_ = Mapper::None;
    bool battery_ = false, hasClock_ = false, rumble_ = false, mbc30_ = false, multicart_ = false;
    size_t romMask_ = 0;  // ROM size - 1, a power of two
    bool changed_ = false;

    bool ramEnabled_ = false;
    uint8_t bank1_ = 1, bank2_ = 0, mode_ = 0;  // MBC1
    uint16_t romBank_ = 1;                      // MBC2, MBC3, MBC5
    uint8_t ramBank_ = 0;                       // MBC3 (8-C: the clock), MBC5
    uint8_t latch_ = 0xFF;
    // Seconds, minutes, hours, day low, day high (bit 0: day 8, bit 6: halt, bit 7: carry).
    uint8_t rtc_[5] = {}, rtcLatched_[5] = {};
    int32_t rtcCycles_ = 0;
};

}  // namespace plumnin::gb
