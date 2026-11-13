#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "snes_copro.h"
#include "snes_cpu.h"

namespace plumnin::snes {

// The SA-1 (Super Mario RPG, Kirby Super Star, Kirby's Dream Land 3...): a second
// 65C816 at 10.7 MHz on the cartridge, with 2 KB of its own RAM (I-RAM) and the
// cartridge's battery RAM (BW-RAM), shared with the console's processor. Around it: the
// mapping of the ROM in 1 MB banks, a multiplier and divider, a reader of bit fields,
// timers, interrupts both ways, and a DMA that can turn bitmaps into SNES tiles.
//
// It runs in step with the console's processor; when both reach the ROM, BW-RAM or
// I-RAM at the same time the SA-1 waits.
class Sa1 : public Coprocessor {
public:
    // The cartridge's ROM and BW-RAM, shared with it.
    Sa1(const std::vector<uint8_t>& rom, std::vector<uint8_t>& bwram);

    void power() override;
    void reset() override;
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t clocks) override { budget_ += clocks; }
    void setMasterRate(double hz) override { scanlines_ = hz < 21400000 ? 312 : 262; }
    void catchUp() override;
    bool irq() const override {
        return (io_.cpuIrqen && io_.cpuIrqfl) || (io_.chdmaIrqen && io_.chdmaIrqfl);
    }
    void cpuAccess(uint32_t address) override { cpuMar_ = address; }
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }

    const Cpu::Regs& cpuRegs() const { return cpu_.r; }
    uint8_t iram(int i) const { return iram_[static_cast<size_t>(i) & 0x7FF]; }

private:
    struct Io {
        // CCNT and the rest of the console's side.
        bool sa1Irq = false, sa1Rdyb = false, sa1Resb = true, sa1Nmi = false;
        uint8_t smeg = 0;
        bool cpuIrqen = false, chdmaIrqen = false, cpuIrqcl = false, chdmaIrqcl = false;
        uint16_t crv = 0, cnv = 0, civ = 0;
        bool cpuIrq = false, cpuIvsw = false, cpuNvsw = false;
        uint8_t cmeg = 0;
        bool sa1Irqen = false, timerIrqen = false, dmaIrqen = false, sa1Nmien = false;
        bool sa1Irqcl = false, timerIrqcl = false, dmaIrqcl = false, sa1Nmicl = false;
        uint16_t snv = 0, siv = 0;
        bool hvselb = false, ven = false, hen = false;
        uint16_t hcnt = 0, vcnt = 0;
        std::array<bool, 4> bankMode{};
        std::array<uint8_t, 4> bank{0, 1, 2, 3};
        uint8_t sbm = 0;
        bool sw46 = false;
        uint8_t cbm = 0;
        bool swen = false, cwen = false;
        uint8_t bwp = 0x0F, siwp = 0, ciwp = 0;
        bool dmaen = false, dprio = false, cden = false, cdsel = false, dd = false;
        uint8_t sd = 0;
        bool chdend = false;
        uint8_t dmasize = 0, dmacb = 0;
        uint32_t dsa = 0, dda = 0;
        uint16_t dtc = 0;
        bool bbf = false;
        std::array<uint8_t, 16> brf{};
        bool acm = false, md = false;
        uint16_t ma = 0, mb = 0;
        bool hl = false;
        uint8_t vb = 16;
        uint32_t va = 0;
        uint8_t vbit = 0;
        bool cpuIrqfl = false, chdmaIrqfl = false;
        bool sa1Irqfl = false, timerIrqfl = false, dmaIrqfl = false, sa1Nmifl = false;
        uint16_t hcr = 0, vcr = 0;
        uint64_t mr = 0;
        bool overflow = false;

        template <class Ar>
        void state(Ar& ar) {
            ar(sa1Irq, sa1Rdyb, sa1Resb, sa1Nmi, smeg, cpuIrqen, chdmaIrqen, cpuIrqcl, chdmaIrqcl, crv, cnv, civ);
            ar(cpuIrq, cpuIvsw, cpuNvsw, cmeg, sa1Irqen, timerIrqen, dmaIrqen, sa1Nmien, sa1Irqcl, timerIrqcl, dmaIrqcl,
               sa1Nmicl, snv, siv, hvselb, ven, hen, hcnt, vcnt, bankMode, bank, sbm, sw46, cbm, swen, cwen, bwp, siwp,
               ciwp);
            ar(dmaen, dprio, cden, cdsel, dd, sd, chdend, dmasize, dmacb, dsa, dda, dtc, bbf, brf, acm, md, ma, mb, hl, vb,
               va, vbit, cpuIrqfl, chdmaIrqfl, sa1Irqfl, timerIrqfl, dmaIrqfl, sa1Nmifl, hcr, vcr, mr, overflow);
        }
    };

    // The SA-1's own bus, as its processor drives it.
    struct Bus : CpuBus {
        Sa1* sa1 = nullptr;
        uint8_t read(uint32_t address) override { return sa1->busRead(address); }
        void write(uint32_t address, uint8_t value) override { sa1->busWrite(address, value); }
        void idle() override { sa1->tick(2); }
        bool hardwareVector(bool nmi, uint16_t& pc) override {
            pc = nmi ? sa1->io_.cnv : sa1->io_.civ;
            return true;
        }
    };
    uint8_t busRead(uint32_t address);
    void busWrite(uint32_t address, uint8_t value);

    void tick(int clocks);
    void triggerTimer();
    void updateInterrupts();

    // What the console's processor is reaching now, for the waits.
    bool romConflict() const { return (cpuMar_ & 0x408000) == 0x008000 || (cpuMar_ & 0xC00000) == 0xC00000; }
    bool bwramConflict() const { return (cpuMar_ & 0x40E000) == 0x006000 || (cpuMar_ & 0xF00000) == 0x400000; }
    bool iramConflict() const { return (cpuMar_ & 0x40F800) == 0x003000; }

    uint8_t romRead(uint32_t address) const;  // as the console's processor sees it
    uint8_t romReadSa1(uint32_t address) const;
    uint8_t bwRead(uint32_t address) const;
    void bwWrite(uint32_t address, uint8_t value);
    uint8_t bwReadLinear(uint32_t address) const { return bwRead(address); }
    void bwWriteLinear(uint32_t address, uint8_t value);
    uint8_t bwReadBitmap(uint32_t address) const;
    void bwWriteBitmap(uint32_t address, uint8_t value);
    uint8_t bwReadSa1(uint32_t address) const;
    void bwWriteSa1(uint32_t address, uint8_t value);
    uint8_t readVbr(uint32_t address) const;

    uint8_t ioReadCpu(uint16_t address);
    uint8_t ioReadSa1(uint16_t address);
    void ioWriteCpu(uint16_t address, uint8_t value);
    void ioWriteSa1(uint16_t address, uint8_t value);
    void ioWriteShared(uint16_t address, uint8_t value);

    void dmaNormal();
    void dmaCc1();
    uint8_t dmaCc1Read(uint32_t address);
    void dmaCc2();

    template <class Ar>
    void state(Ar& ar) {
        cpu_.state(ar);
        io_.state(ar);
        ar(iram_, hcounter_, vcounter_, scanlines_, dmaLine_, bwramDma_, cpuMar_, mdr_, budget_);
    }

    const std::vector<uint8_t>& rom_;
    std::vector<uint8_t>& bwram_;
    Bus bus_;
    Cpu cpu_;
    Io io_;
    std::array<uint8_t, 0x800> iram_{};
    uint16_t hcounter_ = 0, vcounter_ = 0, scanlines_ = 262;
    uint8_t dmaLine_ = 0;
    bool bwramDma_ = false;
    uint32_t cpuMar_ = 0;
    uint8_t mdr_ = 0;
    int64_t budget_ = 0;
};

}  // namespace plumnin::snes
