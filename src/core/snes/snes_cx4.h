#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// The Cx4 (Mega Man X2 and X3): Capcom's name for Hitachi's HG51B169, a 24-bit
// processor at 20 MHz that draws the wireframes and rotates the sprites. Its program is
// in the game's ROM, loaded 512 bytes at a time into one of its two caches; it has a
// 24 x 24 multiplier, 3 KB of data RAM, a DMA, and a 1024-word table of constants in
// the chip (sines, reciprocals...), which is the firmware file (cx4.rom, 3 KB).
//
// While it works it owns the ROM: the console's processor runs from WRAM then, and its
// vectors come from the chip's registers (7F60-7F7F).
class Cx4 : public Coprocessor {
public:
    Cx4(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram) : rom_(rom), ram_(ram) {}
    // The table of constants: 1024 words of 3 bytes, low byte first.
    bool setFirmware(const std::vector<uint8_t>& firmware);

    void power() override;
    void reset() override { power(); }
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t clocks) override { budget_ += static_cast<int64_t>(clocks) * kHz; }
    void setMasterRate(double hz) override { masterHz_ = static_cast<int64_t>(hz); }
    void catchUp() override;
    bool irq() const override { return irqLine_; }
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }

    uint32_t gpr(int n) const { return r_.gpr[static_cast<size_t>(n) & 15]; }
    bool busy() const { return io_.cacheEnable || io_.dmaEnable || io_.busPending; }

private:
    static constexpr int64_t kHz = 20000000;

    struct Regs {
        uint16_t pb = 0;  // 15 bits: the program's page
        uint8_t pc = 0;
        bool n = false, z = false, c = false, v = false, i = false;
        uint32_t a = 0;
        uint16_t p = 0;  // 15 bits
        uint64_t mul = 0;  // 48 bits
        uint32_t mdr = 0, rom = 0, ram = 0, mar = 0, dpr = 0;
        std::array<uint32_t, 16> gpr{};
        template <class Ar>
        void state(Ar& ar) {
            ar(pb, pc, n, z, c, v, i, a, p, mul, mdr, rom, ram, mar, dpr, gpr);
        }
    };
    struct Io {
        bool lock = false, halt = true, irq = false, rom = true;
        std::array<uint8_t, 32> vector{};
        uint8_t waitRom = 3, waitRam = 3;
        bool suspend = false;
        uint8_t suspendFor = 0;
        bool cacheEnable = false, cachePage = false, preload = false;
        std::array<bool, 2> cacheLock{};
        std::array<uint32_t, 2> cacheAddress{};
        uint32_t cacheBase = 0;
        uint16_t cachePb = 0;
        uint8_t cachePc = 0;
        bool dmaEnable = false;
        uint32_t dmaSource = 0, dmaTarget = 0;
        uint16_t dmaLength = 0;
        bool busEnable = false, busReading = false, busWriting = false;
        uint8_t busPending = 0;
        uint32_t busAddress = 0;
        template <class Ar>
        void state(Ar& ar) {
            ar(lock, halt, irq, rom, vector, waitRom, waitRam, suspend, suspendFor, cacheEnable, cachePage, preload,
               cacheLock, cacheAddress, cacheBase, cachePb, cachePc);
            ar(dmaEnable, dmaSource, dmaTarget, dmaLength, busEnable, busReading, busWriting, busPending, busAddress);
        }
    };

    // Its view of the bus (the same as the console's, LoROM).
    static bool isRom(uint32_t address) {
        return (address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000;
    }
    bool isRam(uint32_t address) const { return !ram_.empty() && (address & 0xF88000) == 0x700000; }
    static bool isDataRam(uint32_t address) {
        return (address & 0x40E000) == 0x006000 && (address & 0x0C00) != 0x0C00;
    }
    static bool isIo(uint32_t address) { return (address & 0x40EC00) == 0x006C00; }
    uint8_t romRead(uint32_t address) const;
    uint8_t busRead(uint32_t address);
    void busWrite(uint32_t address, uint8_t value);
    uint8_t ioRead(uint16_t address) const;
    void ioWrite(uint16_t address, uint8_t value);

    void stepCycles(uint32_t cycles);
    uint32_t wait(uint32_t address) const;
    void halt();
    void runOne();
    void execute();
    void advance();
    bool cache();
    void dma();
    uint32_t readRegister(uint8_t reg);
    void writeRegister(uint8_t reg, uint32_t value);
    void instruction(uint16_t op);
    void push();
    void pull();
    uint32_t add(uint32_t x, uint32_t y);
    uint32_t sub(uint32_t x, uint32_t y);
    uint32_t logic(uint32_t x);
    void rdram(int byte, uint32_t address);
    void wrram(int byte, uint32_t address);

    template <class Ar>
    void state(Ar& ar) {
        r_.state(ar);
        io_.state(ar);
        ar(programRam_[0], programRam_[1], dataRam_, stack_, irqLine_, budget_);
    }

    const std::vector<uint8_t>& rom_;
    std::vector<uint8_t>& ram_;
    std::array<uint32_t, 1024> dataRom_{};
    std::array<std::array<uint16_t, 256>, 2> programRam_{};
    std::array<uint8_t, 3072> dataRam_{};
    std::array<uint32_t, 8> stack_{};
    Regs r_;
    Io io_;
    bool irqLine_ = false;
    int64_t masterHz_ = 21477272, budget_ = 0;
};

}  // namespace plumnin::snes
