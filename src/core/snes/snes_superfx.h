#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// The Super FX (the GSU, and the GSU-2 of the later games): a 16-bit RISC processor
// that draws into the cartridge's RAM as SNES tiles while the console's processor
// waits, for Star Fox, Yoshi's Island, Doom... It has sixteen registers (R15 is its
// program counter), a 512-byte instruction cache, buffered reads of the ROM and
// writes to the RAM, and the PLOT instruction that turns pixels into bitplanes.
//
// It runs on the master clock (an instruction from the cache takes 2 clocks, 1 at
// the GSU-2's 21 MHz) in step with the processor, which only reaches the ROM and the
// RAM while the GSU leaves them to it.
class SuperFx : public Coprocessor {
public:
    // The cartridge's ROM and RAM, shared with it.
    SuperFx(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram);

    void power() override;
    void reset() override;
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t clocks) override { budget_ += clocks; }
    void setMasterRate(double) override {}
    void catchUp() override;
    bool irq() const override { return irq_; }
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }

    bool running() const { return g_; }
    uint16_t reg(int n) const { return r_[static_cast<size_t>(n & 15)]; }

private:
    struct PixelCache {
        uint16_t offset = 0xFFFF;
        uint8_t bitpend = 0;
        std::array<uint8_t, 8> data{};
    };

    // The GSU's own bus.
    uint8_t busRead(uint32_t address);
    void busWrite(uint32_t address, uint8_t value);
    uint8_t ioRead(uint16_t address);
    void ioWrite(uint16_t address, uint8_t value);

    void step(int clocks);
    int fast(int clsr1, int clsr0) const { return clsr_ ? clsr1 : clsr0; }
    void execute();
    void instruction(uint8_t op);
    uint8_t readOpcode(uint16_t address);
    uint8_t pipe();
    void flushCache() { cacheValid_.fill(false); }

    void syncRom() {
        if (romcl_) step(romcl_);
    }
    uint8_t readRomBuffer() {
        syncRom();
        return romdr_;
    }
    void updateRomBuffer() {
        sfrR_ = true;
        romcl_ = fast(5, 6);
    }
    void syncRam() {
        if (ramcl_) step(ramcl_);
    }
    uint8_t readRamBuffer(uint16_t address) {
        syncRam();
        return busRead(0x700000u + (static_cast<uint32_t>(rambr_) << 16) + address);
    }
    void writeRamBuffer(uint16_t address, uint8_t value) {
        syncRam();
        ramcl_ = fast(5, 6);
        ramar_ = address;
        ramdr_ = value;
    }

    uint8_t color(uint8_t source) const;
    void plot(uint8_t x, uint8_t y);
    uint8_t rpix(uint8_t x, uint8_t y);
    uint32_t tileAddress(uint8_t x, uint8_t y) const;
    void flushPixels(PixelCache& c);

    void set(int n, uint16_t v) {
        r_[static_cast<size_t>(n)] = v;
        if (n == 14) mod14_ = true;
        if (n == 15) mod15_ = true;
    }
    uint16_t sr() const { return r_[sreg_]; }
    void setDr(uint16_t v) { set(dreg_, v); }
    void resetPrefix() {
        b_ = alt1_ = alt2_ = false;
        sreg_ = dreg_ = 0;
    }
    void setSz(uint16_t v) {
        s_ = v & 0x8000;
        z_ = v == 0;
    }
    uint16_t sfr() const;
    void setSfr(uint16_t v);

    template <class Ar>
    void state(Ar& ar) {
        ar(r_, mod14_, mod15_, z_, cy_, s_, ov_, g_, sfrR_, alt1_, alt2_, il_, ih_, b_, irq_);
        ar(pbr_, rombr_, rambr_, cbr_, scbr_, scmr_, colr_, por_, bramr_, cfgr_, clsr_);
        ar(pipeline_, ramaddr_, romcl_, romdr_, ramcl_, ramar_, ramdr_, sreg_, dreg_, cache_, budget_);
        for (bool& v : cacheValid_) ar(v);
        for (PixelCache& p : pixels_) ar(p.offset, p.bitpend, p.data);
    }

    const std::vector<uint8_t>& rom_;
    std::vector<uint8_t>& ram_;
    uint32_t romMask_ = 0, ramMask_ = 0;

    std::array<uint16_t, 16> r_{};
    bool mod14_ = false, mod15_ = false;
    bool z_ = false, cy_ = false, s_ = false, ov_ = false, g_ = false, sfrR_ = false;
    bool alt1_ = false, alt2_ = false, il_ = false, ih_ = false, b_ = false, irq_ = false;
    uint8_t pbr_ = 0, rombr_ = 0;
    bool rambr_ = false;
    uint16_t cbr_ = 0;
    uint8_t scbr_ = 0, scmr_ = 0, colr_ = 0, por_ = 0;
    bool bramr_ = false;
    uint8_t cfgr_ = 0;
    bool clsr_ = false;

    uint8_t pipeline_ = 1;
    uint16_t ramaddr_ = 0;
    int32_t romcl_ = 0;
    uint8_t romdr_ = 0;
    int32_t ramcl_ = 0;
    uint16_t ramar_ = 0;
    uint8_t ramdr_ = 0;
    uint8_t sreg_ = 0, dreg_ = 0;

    std::array<uint8_t, 512> cache_{};
    std::array<bool, 32> cacheValid_{};
    std::array<PixelCache, 2> pixels_{};

    int64_t budget_ = 0;  // master clocks it is behind the processor
};

}  // namespace plumnin::snes
