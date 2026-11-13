#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../state_io.h"

namespace plumnin::snes {

// How the cartridge's ROM appears in the processor's banks.
enum class Mapping : uint8_t { LoRom, HiRom, ExHiRom };

// What the cartridge's header (at 7FC0, FFC0 or 40FFC0 of the ROM) says.
struct CartInfo {
    std::string title;
    Mapping mapping = Mapping::LoRom;
    bool fastRom = false;
    bool battery = false;
    bool pal = false;
    uint32_t ramSize = 0;
    uint8_t chipset = 0;  // FFD6: the extra chips (0-2 none, else a coprocessor)
    // The coprocessor the game needs, which plumnin does not emulate ("" if none).
    std::string coprocessor;
};

// The ROM (without a copier's header) and the battery-backed RAM, as the address bus
// reaches them.
class Cart {
public:
    // False, with the reason in Spanish, if it is not a SNES ROM.
    bool load(const std::vector<uint8_t>& file, std::string* error);
    const CartInfo& info() const { return info_; }
    const std::vector<uint8_t>& rom() const { return rom_; }
    std::vector<uint8_t>& ram() { return ram_; }
    const std::vector<uint8_t>& ram() const { return ram_; }

    // The cartridge's answer at that address, or false if it does not drive the bus.
    bool read(uint32_t address, uint8_t& value) const;
    bool write(uint32_t address, uint8_t value);
    // Where in the ROM that address lands, -1 if not in the ROM (for the Game Genie).
    int64_t romOffset(uint32_t address) const;
    bool ramChanged() const { return ramChanged_; }
    void clearRamChanged() { ramChanged_ = false; }

private:
    int64_t ramOffset(uint32_t address) const;
    uint32_t mirror(uint32_t offset, uint32_t size) const;

    CartInfo info_;
    std::vector<uint8_t> rom_, ram_;
    bool ramChanged_ = false;
};

}  // namespace plumnin::snes
