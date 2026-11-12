#include "nes_cart.h"

#include <algorithm>
#include <cstring>

#include "../i18n.h"

namespace plumnin::nes {

void Mapper::reset() {
    setPrg16(0, 0);
    setPrg16(1, -1);
    setChr8(0);
    setMirroring(info_.mirroring);
    ramEnabled_ = ramWritable_ = true;
    ramOffset_ = 0;
}

void Mapper::setPrg8(int slot, int bank) {
    const int n = prgBanks8();
    bank = ((bank % n) + n) % n;
    prgMap_[static_cast<size_t>(slot)] = static_cast<uint32_t>(bank) * 0x2000;
}

void Mapper::setChr1(int slot, int bank) {
    const int n = chrBanks1();
    bank = ((bank % n) + n) % n;
    chrMap_[static_cast<size_t>(slot)] = static_cast<uint32_t>(bank) * 0x400;
}

void Mapper::setMirroring(Mirroring m) {
    switch (m) {
        case Mirroring::Horizontal: ntMap_ = {0, 0, 1, 1}; break;
        case Mirroring::Vertical: ntMap_ = {0, 1, 0, 1}; break;
        case Mirroring::SingleA: ntMap_ = {0, 0, 0, 0}; break;
        case Mirroring::SingleB: ntMap_ = {1, 1, 1, 1}; break;
        case Mirroring::FourScreen: ntMap_ = {0, 1, 2, 3}; break;
    }
}

uint8_t Mapper::cpuRead(uint16_t address, uint8_t openBus) {
    if (address >= 0x8000) return prgByte(address);
    if (address >= 0x6000 && !prgRam_.empty() && ramEnabled_)
        return prgRam_[(ramOffset_ + (address & 0x1FFF)) % prgRam_.size()];
    return openBus;
}

void Mapper::cpuWrite(uint16_t address, uint8_t value) {
    if (address >= 0x6000 && address < 0x8000 && !prgRam_.empty() && ramEnabled_ && ramWritable_) {
        uint8_t& b = prgRam_[(ramOffset_ + (address & 0x1FFF)) % prgRam_.size()];
        if (b != value) ramChanged_ = true;
        b = value;
    }
}

namespace {

// 0: NROM, and the default behaviour of every mapper.
class Nrom : public Mapper {};

// 1: MMC1 (SxROM): five writes through a shift register load each register.
class Mmc1 : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        shift_ = 0x10;
        control_ = 0x0C;
        chr0_ = chr1_ = prgReg_ = 0;
        cycle_ = lastWrite_ = 0;
        update();
    }
    void cpuClock() override { ++cycle_; }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) {
            Mapper::cpuWrite(address, value);
            return;
        }
        // Writes on consecutive cycles (a read-modify-write's two) only count once.
        const bool consecutive = cycle_ == lastWrite_ + 1;
        lastWrite_ = cycle_;
        if (value & 0x80) {
            shift_ = 0x10;
            control_ |= 0x0C;
            update();
            return;
        }
        if (consecutive) return;
        const bool full = shift_ & 1;
        shift_ = static_cast<uint8_t>((shift_ >> 1) | (value & 1) << 4);
        if (!full) return;
        switch ((address >> 13) & 3) {
            case 0: control_ = shift_; break;
            case 1: chr0_ = shift_; break;
            case 2: chr1_ = shift_; break;
            default: prgReg_ = shift_; break;
        }
        shift_ = 0x10;
        update();
    }

private:
    void update() {
        static const Mirroring mirrors[4] = {Mirroring::SingleA, Mirroring::SingleB, Mirroring::Vertical,
                                             Mirroring::Horizontal};
        if (info_.mirroring != Mirroring::FourScreen) setMirroring(mirrors[control_ & 3]);
        // SUROM and SXROM: CHR bank 0's bit 4 picks the 256 KB half of a 512 KB PRG ROM.
        const int outer = prg_.size() > 0x40000 ? (chr0_ & 0x10) : 0;
        const int bank = prgReg_ & 0x0F;
        switch ((control_ >> 2) & 3) {
            case 0:
            case 1: setPrg16(0, outer | (bank & 0x0E)), setPrg16(1, outer | (bank & 0x0E) | 1); break;
            case 2: setPrg16(0, outer), setPrg16(1, outer | bank); break;
            default: setPrg16(0, outer | bank), setPrg16(1, outer | 0x0F); break;
        }
        if (control_ & 0x10) setChr4(0, chr0_), setChr4(1, chr1_);
        else setChr8(chr0_ >> 1);
        ramEnabled_ = !(prgReg_ & 0x10);
        // SOROM and SXROM: 16 or 32 KB of PRG RAM, banked by CHR bank 0's bits 2-3.
        if (prgRam_.size() > 0x2000) ramOffset_ = static_cast<uint32_t>(((chr0_ >> 2) & 3) * 0x2000) % prgRam_.size();
    }

    uint8_t shift_ = 0x10, control_ = 0x0C, chr0_ = 0, chr1_ = 0, prgReg_ = 0;
    uint64_t cycle_ = 0, lastWrite_ = 0;
    NES_MAPPER_STATE(shift_, control_, chr0_, chr1_, prgReg_, cycle_, lastWrite_)
};

// 2: UxROM.
class Uxrom : public Mapper {
public:
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address >= 0x8000) setPrg16(0, value);
        else Mapper::cpuWrite(address, value);
    }
};

// 3: CNROM.
class Cnrom : public Mapper {
public:
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address >= 0x8000) setChr8(value);
        else Mapper::cpuWrite(address, value);
    }
};

// 4: MMC3 (TxROM), with its scanline counter clocked by A12 rising on the PPU bus.
class Mmc3 : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        select_ = 0;
        regs_ = {0, 2, 4, 5, 6, 7, 0, 1};
        latch_ = counter_ = 0;
        reload_ = irqEnabled_ = irq_ = false;
        a12_ = false;
        lowSince_ = 0;
        update();
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) {
            Mapper::cpuWrite(address, value);
            return;
        }
        const bool odd = address & 1;
        switch (address & 0xE000) {
            case 0x8000:
                if (odd) regs_[select_ & 7] = value;
                else select_ = value;
                update();
                break;
            case 0xA000:
                if (!odd) {
                    if (info_.mirroring != Mirroring::FourScreen)
                        setMirroring(value & 1 ? Mirroring::Horizontal : Mirroring::Vertical);
                } else {
                    ramEnabled_ = value & 0x80;
                    ramWritable_ = !(value & 0x40);
                }
                break;
            case 0xC000:
                if (odd) counter_ = 0, reload_ = true;
                else latch_ = value;
                break;
            default:
                irqEnabled_ = odd;
                if (!odd) irq_ = false;
                break;
        }
    }
    bool watchesPpu() const override { return true; }
    void ppuAddress(uint16_t address, uint64_t dot) override {
        const bool a12 = address & 0x1000;
        // A12 must have stayed low a while (the chip filters the short drops of the
        // sprite fetches) for its rise to clock the counter.
        if (a12 && !a12_ && dot - lowSince_ >= 10) clock();
        if (!a12 && a12_) lowSince_ = dot;
        a12_ = a12;
    }
    bool irq() const override { return irq_; }

protected:
    void clock() {
        const bool natural = counter_ == 0 && !reload_;
        if (counter_ == 0 || reload_) {
            counter_ = latch_;
            reload_ = false;
        } else {
            --counter_;
        }
        // The older MMC3A (NES 2.0 submapper 4) gives no IRQ when it reloads a 0 after
        // having counted down to 0 by itself.
        if (counter_ == 0 && irqEnabled_ && !(natural && info_.submapper == 4)) irq_ = true;
    }
    virtual void update() {
        if (select_ & 0x40) setPrg8(0, -2), setPrg8(2, regs_[6]);
        else setPrg8(0, regs_[6]), setPrg8(2, -2);
        setPrg8(1, regs_[7]);
        setPrg8(3, -1);
        const int flip = select_ & 0x80 ? 4 : 0;
        setChr1(0 ^ flip, regs_[0] & 0xFE);
        setChr1(1 ^ flip, regs_[0] | 1);
        setChr1(2 ^ flip, regs_[1] & 0xFE);
        setChr1(3 ^ flip, regs_[1] | 1);
        setChr1(4 ^ flip, regs_[2]);
        setChr1(5 ^ flip, regs_[3]);
        setChr1(6 ^ flip, regs_[4]);
        setChr1(7 ^ flip, regs_[5]);
    }

    uint8_t select_ = 0;
    std::array<uint8_t, 8> regs_{};
    uint8_t latch_ = 0, counter_ = 0;
    bool reload_ = false, irqEnabled_ = false, irq_ = false, a12_ = false;
    uint64_t lowSince_ = 0;
    NES_MAPPER_STATE(select_, regs_, latch_, counter_, reload_, irqEnabled_, irq_, a12_, lowSince_)
};

// 206: Namco 108 (DxROM), the MMC3's ancestor: no IRQ, fixed mirroring.
class Namco108 : public Mmc3 {
public:
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000 || address >= 0xA000) return;
        if (address & 1) regs_[select_ & 7] = value & 0x3F;
        else select_ = value & 7;
        update();
    }
    bool watchesPpu() const override { return false; }
    void ppuAddress(uint16_t, uint64_t) override {}
};

// 7: AxROM: 32 KB banks and one-screen mirroring.
class Axrom : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        setPrg32(0);
        setMirroring(Mirroring::SingleA);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) return;
        setPrg32(value & 0x0F);
        setMirroring(value & 0x10 ? Mirroring::SingleB : Mirroring::SingleA);
    }
};

// 9 and 10: MMC2 (Punch-Out!!) and MMC4: CHR banks switched by the PPU reading
// tiles FD and FE.
class Mmc2 : public Mapper {
public:
    explicit Mmc2(bool mmc4) : mmc4_(mmc4) {}
    void reset() override {
        Mapper::reset();
        prgReg_ = 0;
        chrReg_ = {0, 0, 0, 0};
        latch_ = {0, 0};
        update();
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0xA000) {
            Mapper::cpuWrite(address, value);
            return;
        }
        switch (address & 0xF000) {
            case 0xA000: prgReg_ = value & 0x0F; break;
            case 0xB000: chrReg_[0] = value & 0x1F; break;
            case 0xC000: chrReg_[1] = value & 0x1F; break;
            case 0xD000: chrReg_[2] = value & 0x1F; break;
            case 0xE000: chrReg_[3] = value & 0x1F; break;
            default: setMirroring(value & 1 ? Mirroring::Horizontal : Mirroring::Vertical); break;
        }
        update();
    }
    bool watchesPpu() const override { return true; }
    void ppuAddress(uint16_t address, uint64_t) override {
        const uint16_t a = address & 0x1FF8;
        if (mmc4_ ? a == 0x0FD8 : (address & 0x1FFF) == 0x0FD8) latch_[0] = 0;
        else if (mmc4_ ? a == 0x0FE8 : (address & 0x1FFF) == 0x0FE8) latch_[0] = 1;
        else if (a == 0x1FD8) latch_[1] = 0;
        else if (a == 0x1FE8) latch_[1] = 1;
        else return;
        update();
    }

private:
    void update() {
        if (mmc4_) {
            setPrg16(0, prgReg_);
            setPrg16(1, -1);
        } else {
            setPrg8(0, prgReg_);
            setPrg8(1, -3);
            setPrg8(2, -2);
            setPrg8(3, -1);
        }
        setChr4(0, chrReg_[latch_[0]]);
        setChr4(1, chrReg_[2 + latch_[1]]);
    }
    bool mmc4_;
    uint8_t prgReg_ = 0;
    std::array<uint8_t, 4> chrReg_{};
    std::array<uint8_t, 2> latch_{};
    NES_MAPPER_STATE(prgReg_, chrReg_, latch_)
};

// 11: Color Dreams.
class ColorDreams : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        setPrg32(0);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) return;
        setPrg32(value & 3);
        setChr8(value >> 4);
    }
};

// 34: BNROM (32 KB PRG banks) or, with CHR ROM, NINA-001.
class Bnrom : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        setPrg32(0);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address >= 0x8000) {
            if (chrIsRam_) setPrg32(value);
            return;
        }
        Mapper::cpuWrite(address, value);
        if (chrIsRam_) return;
        if (address == 0x7FFD) setPrg32(value & 1);
        else if (address == 0x7FFE) setChr4(0, value & 0x0F);
        else if (address == 0x7FFF) setChr4(1, value & 0x0F);
    }
};

// 66: GxROM.
class Gxrom : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        setPrg32(0);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) return;
        setPrg32((value >> 4) & 3);
        setChr8(value & 3);
    }
};

// 71: Camerica / Codemasters.
class Camerica : public Mapper {
public:
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address >= 0xC000) setPrg16(0, value);
        else if (address >= 0x8000 && address < 0xA000 && info_.submapper == 1)
            setMirroring(value & 0x10 ? Mirroring::SingleB : Mirroring::SingleA);
    }
};

// 180: UNROM with the fixed bank at 8000 (Crazy Climber).
class Unrom180 : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        setPrg16(0, 0);
        setPrg16(1, 0);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address >= 0x8000) setPrg16(1, value & 7);
    }
};

// 69: Sunsoft FME-7, with its cycle counter IRQ (its 5B sound is not emulated).
class Fme7 : public Mapper {
public:
    void reset() override {
        Mapper::reset();
        command_ = 0;
        bank6000_ = 0;
        irqControl_ = 0;
        counter_ = 0;
        irq_ = false;
        setPrg8(3, -1);
    }
    uint8_t cpuRead(uint16_t address, uint8_t openBus) override {
        if (address >= 0x6000 && address < 0x8000) {
            if (!(bank6000_ & 0x40)) return prg_[static_cast<size_t>((bank6000_ & 0x3F) % prgBanks8()) * 0x2000 + (address & 0x1FFF)];
            if (!(bank6000_ & 0x80) || prgRam_.empty()) return openBus;
        }
        return Mapper::cpuRead(address, openBus);
    }
    void cpuWrite(uint16_t address, uint8_t value) override {
        if (address < 0x8000) {
            if ((bank6000_ & 0xC0) == 0xC0) Mapper::cpuWrite(address, value);
            return;
        }
        if (address < 0xA000) {
            command_ = value & 0x0F;
            return;
        }
        if (address >= 0xC000) return;  // the 5B's sound
        switch (command_) {
            case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: setChr1(command_, value); break;
            case 8: bank6000_ = value; break;
            case 9: case 10: case 11: setPrg8(command_ - 9, value & 0x3F); break;
            case 12: {
                static const Mirroring m[4] = {Mirroring::Vertical, Mirroring::Horizontal, Mirroring::SingleA,
                                               Mirroring::SingleB};
                setMirroring(m[value & 3]);
                break;
            }
            case 13: irqControl_ = value, irq_ = false; break;
            case 14: counter_ = static_cast<uint16_t>((counter_ & 0xFF00) | value); break;
            default: counter_ = static_cast<uint16_t>((counter_ & 0x00FF) | value << 8); break;
        }
    }
    void cpuClock() override {
        if (!(irqControl_ & 0x80)) return;
        if (--counter_ == 0xFFFF && (irqControl_ & 1)) irq_ = true;
    }
    bool irq() const override { return irq_; }

private:
    uint8_t command_ = 0, bank6000_ = 0, irqControl_ = 0;
    uint16_t counter_ = 0;
    bool irq_ = false;
    NES_MAPPER_STATE(command_, bank6000_, irqControl_, counter_, irq_)
};

}  // namespace

std::unique_ptr<Mapper> Mapper::create(const std::vector<uint8_t>& f, std::string* error) {
    auto fail = [&](std::string why) -> std::unique_ptr<Mapper> {
        if (error) *error = std::move(why);
        return nullptr;
    };
    if (f.size() < 16 || f[0] != 'N' || f[1] != 'E' || f[2] != 'S' || f[3] != 0x1A)
        return fail(tr("no es una ROM de NES (le falta la cabecera iNES)"));
    CartInfo info;
    const bool nes2 = (f[7] & 0x0C) == 0x08;
    size_t prgSize = f[4] * 0x4000ull, chrSize = f[5] * 0x2000ull;
    info.mapper = (f[6] >> 4) | (f[7] & 0xF0);
    if (nes2) {
        info.mapper |= (f[8] & 0x0F) << 8;
        info.submapper = f[8] >> 4;
        prgSize += static_cast<size_t>(f[9] & 0x0F) << 8 << 14;
        chrSize += static_cast<size_t>(f[9] >> 4) << 8 << 13;
        const int ram = f[10] & 0x0F, nvram = f[10] >> 4;
        info.prgRamSize = (ram ? 64u << ram : 0) + (nvram ? 64u << nvram : 0);
        if (!info.prgRamSize) info.prgRamSize = 0x2000;  // most games that leave it out still have it
        if (!chrSize) info.chrRamSize = f[11] & 0x0F ? 64u << (f[11] & 0x0F) : 0x2000;
        info.pal = (f[12] & 3) == 1;
    } else {
        // Old dumps carry junk ("DiskDude!") in bytes 7-15: keep only the low mapper nibble then.
        if (f[12] || f[13] || f[14] || f[15]) info.mapper &= 0x0F;
        if (!chrSize) info.chrRamSize = 0x2000;
        info.pal = f[9] & 1;
    }
    info.battery = f[6] & 2;
    info.mirroring = f[6] & 8 ? Mirroring::FourScreen : f[6] & 1 ? Mirroring::Vertical : Mirroring::Horizontal;
    const bool trainer = f[6] & 4;
    const size_t start = 16 + (trainer ? 512 : 0);
    if (!prgSize) return fail(tr("la ROM de NES no tiene PRG"));
    if (f.size() < start + prgSize + chrSize) return fail(tr("la ROM de NES está cortada"));

    std::unique_ptr<Mapper> m;
    switch (info.mapper) {
        case 0: m = std::make_unique<Nrom>(); break;
        case 1: m = std::make_unique<Mmc1>(); break;
        case 2: m = std::make_unique<Uxrom>(); break;
        case 3: m = std::make_unique<Cnrom>(); break;
        case 4: m = std::make_unique<Mmc3>(); break;
        case 7: m = std::make_unique<Axrom>(); break;
        case 9: m = std::make_unique<Mmc2>(false); break;
        case 10: m = std::make_unique<Mmc2>(true); break;
        case 11: m = std::make_unique<ColorDreams>(); break;
        case 34: m = std::make_unique<Bnrom>(); break;
        case 66: m = std::make_unique<Gxrom>(); break;
        case 69: m = std::make_unique<Fme7>(); break;
        case 71: m = std::make_unique<Camerica>(); break;
        case 180: m = std::make_unique<Unrom180>(); break;
        case 206: m = std::make_unique<Namco108>(); break;
        default: return fail(trf("el cartucho usa el mapper %d, que plumnin no emula todavía", info.mapper));
    }
    m->info_ = info;
    m->prg_.assign(f.begin() + static_cast<long>(start), f.begin() + static_cast<long>(start + prgSize));
    // PRG in whole 8 KB banks.
    if (m->prg_.size() % 0x2000) m->prg_.resize((m->prg_.size() / 0x2000 + 1) * 0x2000, 0xFF);
    if (chrSize) {
        m->chr_.assign(f.begin() + static_cast<long>(start + prgSize), f.begin() + static_cast<long>(start + prgSize + chrSize));
    } else {
        m->chr_.assign(std::max<size_t>(info.chrRamSize, 0x2000), 0);
        m->chrIsRam_ = true;
    }
    m->prgRam_.assign(info.prgRamSize, 0);
    if (trainer) std::copy_n(f.begin() + 16, 512, m->prgRam_.begin() + std::min<size_t>(0x1000, m->prgRam_.size() - 512));
    m->reset();
    return m;
}

}  // namespace plumnin::nes
