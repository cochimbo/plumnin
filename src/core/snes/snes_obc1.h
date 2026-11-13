#pragma once

#include <cstdint>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// The OBC1 (Metal Combat: Falcon's Revenge): it keeps a table of sprites in the
// cartridge's 8 KB of RAM and lets the game reach one entry's four bytes and its two
// extra bits through fixed registers at 7FF0-7FF7. Its whole state is in that RAM (the
// table's base at 7FF5 and the entry at 7FF6), so it saves along with it.
class Obc1 : public Coprocessor {
public:
    explicit Obc1(std::vector<uint8_t>& ram) : ram_(ram) {}

    void power() override {}
    void reset() override {}
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t) override {}
    void setMasterRate(double) override {}
    void catchUp() override {}
    void save(StateWriter&) override {}
    void load(StateReader&) override {}

private:
    static bool mapped(uint32_t address);
    uint8_t ram(uint16_t a) const { return ram_[a % ram_.size()]; }
    void setRam(uint16_t a, uint8_t value);
    uint16_t base() const { return ram(0x1FF5) & 1 ? 0x1800 : 0x1C00; }
    uint8_t index() const { return ram(0x1FF6) & 0x7F; }

    std::vector<uint8_t>& ram_;
};

}  // namespace plumnin::snes
