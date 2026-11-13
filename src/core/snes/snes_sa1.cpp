#include "snes_sa1.h"

namespace plumnin::snes {

namespace {

// A size that is not a power of two repeats its last part, as the chips are wired.
uint32_t mirror(uint32_t offset, uint32_t size) {
    if (!size) return 0;
    uint32_t base = 0, mask = 1u << 23;
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

}  // namespace

Sa1::Sa1(const std::vector<uint8_t>& rom, std::vector<uint8_t>& bwram) : rom_(rom), bwram_(bwram) {
    bus_.sa1 = this;
    cpu_.setBus(&bus_);
}

void Sa1::power() {
    iram_.fill(0);
    reset();
}

void Sa1::reset() {
    io_ = Io{};
    hcounter_ = vcounter_ = 0;
    dmaLine_ = 0;
    bwramDma_ = false;
    mdr_ = 0;
    cpu_.power();  // reads its vector; CCNT gives it the real one when it starts
    budget_ = 0;
}

// ---- Time: two master clocks a cycle, and the timer.

void Sa1::tick(int clocks) {
    budget_ -= clocks;
    for (int c = 0; c < clocks; c += 2) {
        if (!io_.hvselb) {
            hcounter_ = static_cast<uint16_t>(hcounter_ + 2);
            if (hcounter_ >= 1364) {
                hcounter_ = 0;
                if (++vcounter_ >= scanlines_) vcounter_ = 0;
            }
        } else {
            hcounter_ = static_cast<uint16_t>(hcounter_ + 2);
            vcounter_ = static_cast<uint16_t>((vcounter_ + (hcounter_ >> 11)) & 0x1FF);
            hcounter_ &= 0x7FF;
        }
        switch ((io_.hen ? 1 : 0) | (io_.ven ? 2 : 0)) {
            case 1:
                if (hcounter_ == io_.hcnt << 2) triggerTimer();
                break;
            case 2:
                if (vcounter_ == io_.vcnt && hcounter_ == 0) triggerTimer();
                break;
            case 3:
                if (vcounter_ == io_.vcnt && hcounter_ == io_.hcnt << 2) triggerTimer();
                break;
            default: break;
        }
    }
}

void Sa1::triggerTimer() {
    io_.timerIrqfl = true;
    if (io_.timerIrqen) io_.timerIrqcl = false;
}

void Sa1::updateInterrupts() {
    if (io_.sa1Nmi && !io_.sa1Nmicl) {
        cpu_.raiseNmi();
        io_.sa1Nmifl = true;
        io_.sa1Nmicl = true;
    }
    cpu_.setIrq((io_.timerIrqen && !io_.timerIrqcl) || (io_.dmaIrqen && !io_.dmaIrqcl) ||
                (io_.sa1Irq && !io_.sa1Irqcl));
}

void Sa1::catchUp() {
    while (budget_ > 0) {
        if (io_.sa1Rdyb || io_.sa1Resb) {
            // Asleep, or held in reset: only the timer runs.
            tick(static_cast<int>(std::min<int64_t>(budget_ + (budget_ & 1), 64)));
            continue;
        }
        updateInterrupts();
        cpu_.step();
    }
}

// ---- The ROM: four 1 MB windows the game points where it wants.

uint8_t Sa1::romRead(uint32_t address) const {
    if (rom_.empty()) return 0;
    const bool lo = address < 0x400000;  // the banks' modes only change 00-3F and 80-BF
    address &= 0x3FFFFF;
    const int w = static_cast<int>(address >> 20);
    if (!(lo && !io_.bankMode[static_cast<size_t>(w)]))
        address = static_cast<uint32_t>(io_.bank[static_cast<size_t>(w)]) << 20 | (address & 0x0FFFFF);
    return rom_[mirror(address, static_cast<uint32_t>(rom_.size()))];
}

// An address of the bus (00-3F/80-BF:8000-FFFF or C0-FF) as the ROM's windows take it.
uint8_t Sa1::romReadSa1(uint32_t address) const {
    if ((address & 0x408000) == 0x008000)
        address = (address & 0x800000) >> 2 | (address & 0x3F0000) >> 1 | (address & 0x007FFF);
    return romRead(address);
}

// ---- BW-RAM: plain, or as a bitmap of 2 or 4 bits a pixel (60-6F on the SA-1's side).

uint8_t Sa1::bwRead(uint32_t address) const {
    if (bwram_.empty()) return 0;
    return bwram_[mirror(address & 0xFFFFF, static_cast<uint32_t>(bwram_.size()))];
}

void Sa1::bwWrite(uint32_t address, uint8_t value) {
    if (bwram_.empty()) return;
    uint8_t& b = bwram_[mirror(address & 0xFFFFF, static_cast<uint32_t>(bwram_.size()))];
    if (b != value) batteryChanged_ = true;
    b = value;
}

void Sa1::bwWriteLinear(uint32_t address, uint8_t value) {
    // The protected area at its start only holds while both sides have writes off.
    if (!io_.swen && !io_.cwen && (address & 0x3FFFF) < (0x100u << io_.bwp)) return;
    bwWrite(address, value);
}

uint8_t Sa1::bwReadBitmap(uint32_t address) const {
    address &= 0xFFFFF;
    if (!io_.bbf) return static_cast<uint8_t>((bwRead(address >> 1) >> ((address & 1) * 4)) & 0x0F);
    return static_cast<uint8_t>((bwRead(address >> 2) >> ((address & 3) * 2)) & 0x03);
}

void Sa1::bwWriteBitmap(uint32_t address, uint8_t value) {
    address &= 0xFFFFF;
    if (!io_.bbf) {
        const int shift = (address & 1) * 4;
        const uint32_t a = address >> 1;
        bwWrite(a, static_cast<uint8_t>((bwRead(a) & ~(0x0F << shift)) | (value & 0x0F) << shift));
    } else {
        const int shift = (address & 3) * 2;
        const uint32_t a = address >> 2;
        bwWrite(a, static_cast<uint8_t>((bwRead(a) & ~(0x03 << shift)) | (value & 0x03) << shift));
    }
}

uint8_t Sa1::bwReadSa1(uint32_t address) const {
    if (!io_.sw46) return bwReadLinear((io_.cbm & 0x1Fu) * 0x2000 + (address & 0x1FFF));
    return bwReadBitmap(io_.cbm * 0x2000u + (address & 0x1FFF));
}

void Sa1::bwWriteSa1(uint32_t address, uint8_t value) {
    if (!io_.sw46) bwWriteLinear((io_.cbm & 0x1Fu) * 0x2000 + (address & 0x1FFF), value);
    else bwWriteBitmap(io_.cbm * 0x2000u + (address & 0x1FFF), value);
}

uint8_t Sa1::readVbr(uint32_t address) const {
    address &= 0xFFFFFF;
    if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) return romReadSa1(address);
    if ((address & 0x40E000) == 0x006000 || (address & 0xF00000) == 0x400000) return bwRead(address);
    if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) return iram_[address & 0x7FF];
    return 0xFF;
}

// ---- The SA-1's bus.

uint8_t Sa1::busRead(uint32_t address) {
    address &= 0xFFFFFF;
    if ((address & 0x40FE00) == 0x002200) {
        tick(2);
        return mdr_ = ioReadSa1(static_cast<uint16_t>(address));
    }
    if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) {
        tick(romConflict() ? 4 : 2);
        return mdr_ = romReadSa1(address);
    }
    if ((address & 0x40E000) == 0x006000 || (address & 0xE00000) == 0x400000 || (address & 0xF00000) == 0x600000) {
        tick(bwramConflict() ? 8 : 4);
        if ((address & 0x600000) == 0x600000) return mdr_ = bwReadBitmap(address);
        if (address & 0x400000) return mdr_ = bwReadLinear(address);
        return mdr_ = bwReadSa1(address);
    }
    if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) {
        tick(iramConflict() ? 6 : 2);
        return mdr_ = iram_[address & 0x7FF];
    }
    tick(2);
    return mdr_;
}

void Sa1::busWrite(uint32_t address, uint8_t value) {
    address &= 0xFFFFFF;
    mdr_ = value;
    if ((address & 0x40FE00) == 0x002200) {
        tick(2);
        ioWriteSa1(static_cast<uint16_t>(address), value);
        return;
    }
    if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) {
        tick(romConflict() ? 4 : 2);
        return;
    }
    if ((address & 0x40E000) == 0x006000 || (address & 0xE00000) == 0x400000 || (address & 0xF00000) == 0x600000) {
        tick(bwramConflict() ? 8 : 4);
        if ((address & 0x600000) == 0x600000) bwWriteBitmap(address, value);
        else if (address & 0x400000) bwWriteLinear(address, value);
        else bwWriteSa1(address, value);
        return;
    }
    if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) {
        tick(iramConflict() ? 6 : 2);
        if (io_.ciwp & (1 << ((address >> 8) & 7))) iram_[address & 0x7FF] = value;
        return;
    }
    tick(2);
}

// ---- The console's processor's side.

bool Sa1::read(uint32_t address, uint8_t& value) {
    const uint32_t bank = (address >> 16) & 0xFF, a = address & 0xFFFF;
    if (!(bank & 0x40)) {
        if (a >= 0x2200 && a < 0x2400) {
            catchUp();
            value = ioReadCpu(static_cast<uint16_t>(a));
            return true;
        }
        if (a >= 0x3000 && a < 0x3800) {
            catchUp();
            value = iram_[a & 0x7FF];
            return true;
        }
        if (a >= 0x6000 && a < 0x8000) {
            catchUp();
            const uint32_t o = io_.sbm * 0x2000u + (a & 0x1FFF);
            value = bwramDma_ ? dmaCc1Read(o) : bwRead(o);
            return true;
        }
        if (a >= 0x8000) {
            // The SNES's vectors can come from registers.
            if (bank == 0x00 && a >= 0xFFE0) {
                if (io_.cpuNvsw && (a == 0xFFEA || a == 0xFFEB)) {
                    value = static_cast<uint8_t>(io_.snv >> (a & 1 ? 8 : 0));
                    return true;
                }
                if (io_.cpuIvsw && (a == 0xFFEE || a == 0xFFEF)) {
                    value = static_cast<uint8_t>(io_.siv >> (a & 1 ? 8 : 0));
                    return true;
                }
            }
            value = romReadSa1(address & 0xFFFFFF);
            return true;
        }
        return false;
    }
    if (bank >= 0xC0) {
        value = romRead(address & 0xFFFFFF);
        return true;
    }
    if (bank >= 0x40 && bank < 0x50) {
        catchUp();
        value = bwramDma_ ? dmaCc1Read(address & 0xFFFFF) : bwRead(address);
        return true;
    }
    return false;
}

bool Sa1::write(uint32_t address, uint8_t value) {
    const uint32_t bank = (address >> 16) & 0xFF, a = address & 0xFFFF;
    if (!(bank & 0x40)) {
        if (a >= 0x2200 && a < 0x2400) {
            catchUp();
            ioWriteCpu(static_cast<uint16_t>(a), value);
            return true;
        }
        if (a >= 0x3000 && a < 0x3800) {
            catchUp();
            if (io_.siwp & (1 << ((a >> 8) & 7))) iram_[a & 0x7FF] = value;
            return true;
        }
        if (a >= 0x6000 && a < 0x8000) {
            catchUp();
            const uint32_t o = io_.sbm * 0x2000u + (a & 0x1FFF);
            if (!io_.swen && !io_.cwen && (o & 0x3FFFF) < (0x100u << io_.bwp)) return true;
            bwWrite(o, value);
            return true;
        }
        return a >= 0x8000;
    }
    if (bank >= 0x40 && bank < 0x50) {
        catchUp();
        const uint32_t o = address & 0xFFFFF;
        if (!io_.swen && !io_.cwen && (o & 0x3FFFF) < (0x100u << io_.bwp)) return true;
        bwWrite(o, value);
        return true;
    }
    return bank >= 0xC0;
}

// ---- Registers.

uint8_t Sa1::ioReadCpu(uint16_t address) {
    if ((0x2200 | (address & 0x1FF)) == 0x2300) {
        return static_cast<uint8_t>(io_.cmeg | io_.cpuNvsw << 4 | io_.chdmaIrqfl << 5 | io_.cpuIvsw << 6 |
                                    io_.cpuIrqfl << 7);
    }
    return 0;  // open bus, roughly
}

uint8_t Sa1::ioReadSa1(uint16_t address) {
    switch (0x2200 | (address & 0x1FF)) {
        case 0x2301:
            return static_cast<uint8_t>(io_.smeg | io_.sa1Nmifl << 4 | io_.dmaIrqfl << 5 | io_.timerIrqfl << 6 |
                                        io_.sa1Irqfl << 7);
        case 0x2302:
            io_.hcr = static_cast<uint16_t>(hcounter_ >> 2);
            io_.vcr = vcounter_;
            return static_cast<uint8_t>(io_.hcr);
        case 0x2303: return static_cast<uint8_t>(io_.hcr >> 8);
        case 0x2304: return static_cast<uint8_t>(io_.vcr);
        case 0x2305: return static_cast<uint8_t>(io_.vcr >> 8);
        case 0x2306:
        case 0x2307:
        case 0x2308:
        case 0x2309:
        case 0x230A: return static_cast<uint8_t>(io_.mr >> (((address & 0x1FF) - 0x106) * 8));
        case 0x230B: return static_cast<uint8_t>(io_.overflow << 7);
        case 0x230C:
        case 0x230D: {
            uint32_t data = readVbr(io_.va) | readVbr(io_.va + 1) << 8 | readVbr(io_.va + 2) << 16;
            data >>= io_.vbit;
            if ((address & 0x1FF) == 0x10C) return static_cast<uint8_t>(data);
            if (io_.hl) {  // auto-increment
                io_.vbit = static_cast<uint8_t>(io_.vbit + io_.vb);
                io_.va = (io_.va + (io_.vbit >> 3)) & 0xFFFFFF;
                io_.vbit &= 7;
            }
            return static_cast<uint8_t>(data >> 8);
        }
        default: return 0xFF;
    }
}

void Sa1::ioWriteCpu(uint16_t address, uint8_t v) {
    switch (0x2200 | (address & 0x1FF)) {
        case 0x2200:  // CCNT
            if (io_.sa1Resb && !(v & 0x20)) {
                // Out of reset: it starts at CRV.
                cpu_.r.pc = io_.crv;
                cpu_.r.pb = 0;
                cpu_.r.db = 0;
                cpu_.clearStop();
                io_.ciwp = 0;
            }
            io_.smeg = v & 0x0F;
            io_.sa1Nmi = v & 0x10;
            io_.sa1Resb = v & 0x20;
            io_.sa1Rdyb = v & 0x40;
            io_.sa1Irq = v & 0x80;
            if (io_.sa1Irq) {
                io_.sa1Irqfl = true;
                if (io_.sa1Irqen) io_.sa1Irqcl = false;
            }
            if (io_.sa1Nmi) {
                io_.sa1Nmifl = true;
                if (io_.sa1Nmien) io_.sa1Nmicl = false;
            }
            return;
        case 0x2201:  // SIE
            if (!io_.chdmaIrqen && (v & 0x20) && io_.chdmaIrqfl) io_.chdmaIrqcl = false;
            if (!io_.cpuIrqen && (v & 0x80) && io_.cpuIrqfl) io_.cpuIrqcl = false;
            io_.chdmaIrqen = v & 0x20;
            io_.cpuIrqen = v & 0x80;
            return;
        case 0x2202:  // SIC
            io_.chdmaIrqcl = v & 0x20;
            io_.cpuIrqcl = v & 0x80;
            if (io_.chdmaIrqcl) io_.chdmaIrqfl = false;
            if (io_.cpuIrqcl) io_.cpuIrqfl = false;
            return;
        case 0x2203: io_.crv = static_cast<uint16_t>((io_.crv & 0xFF00) | v); return;
        case 0x2204: io_.crv = static_cast<uint16_t>((io_.crv & 0x00FF) | v << 8); return;
        case 0x2205: io_.cnv = static_cast<uint16_t>((io_.cnv & 0xFF00) | v); return;
        case 0x2206: io_.cnv = static_cast<uint16_t>((io_.cnv & 0x00FF) | v << 8); return;
        case 0x2207: io_.civ = static_cast<uint16_t>((io_.civ & 0xFF00) | v); return;
        case 0x2208: io_.civ = static_cast<uint16_t>((io_.civ & 0x00FF) | v << 8); return;
        case 0x2220:
        case 0x2221:
        case 0x2222:
        case 0x2223: {  // CXB-FXB: the ROM's windows
            const size_t w = (address & 0x1FF) - 0x20;
            io_.bank[w] = v & 7;
            io_.bankMode[w] = v & 0x80;
            return;
        }
        case 0x2224: io_.sbm = v & 0x1F; return;
        case 0x2226: io_.swen = v & 0x80; return;
        case 0x2228: io_.bwp = v & 0x0F; return;
        case 0x2229: io_.siwp = v; return;
        case 0x2231:
        case 0x2232:
        case 0x2233:
        case 0x2234:
        case 0x2235:
        case 0x2236:
        case 0x2237: ioWriteShared(address, v); return;
        default: return;
    }
}

void Sa1::ioWriteSa1(uint16_t address, uint8_t v) {
    switch (0x2200 | (address & 0x1FF)) {
        case 0x2209:  // SCNT
            io_.cmeg = v & 0x0F;
            io_.cpuNvsw = v & 0x10;
            io_.cpuIvsw = v & 0x40;
            io_.cpuIrq = v & 0x80;
            if (io_.cpuIrq) {
                io_.cpuIrqfl = true;
                if (io_.cpuIrqen) io_.cpuIrqcl = false;
            }
            return;
        case 0x220A:  // CIE
            if (!io_.sa1Nmien && (v & 0x10) && io_.sa1Nmifl) io_.sa1Nmicl = false;
            if (!io_.dmaIrqen && (v & 0x20) && io_.dmaIrqfl) io_.dmaIrqcl = false;
            if (!io_.timerIrqen && (v & 0x40) && io_.timerIrqfl) io_.timerIrqcl = false;
            if (!io_.sa1Irqen && (v & 0x80) && io_.sa1Irqfl) io_.sa1Irqcl = false;
            io_.sa1Nmien = v & 0x10;
            io_.dmaIrqen = v & 0x20;
            io_.timerIrqen = v & 0x40;
            io_.sa1Irqen = v & 0x80;
            return;
        case 0x220B:  // CIC
            io_.sa1Nmicl = v & 0x10;
            io_.dmaIrqcl = v & 0x20;
            io_.timerIrqcl = v & 0x40;
            io_.sa1Irqcl = v & 0x80;
            if (io_.sa1Nmicl) io_.sa1Nmifl = false;
            if (io_.sa1Irqcl) io_.sa1Irqfl = false;
            if (io_.timerIrqcl) io_.timerIrqfl = false;
            if (io_.dmaIrqcl) io_.dmaIrqfl = false;
            return;
        case 0x220C: io_.snv = static_cast<uint16_t>((io_.snv & 0xFF00) | v); return;
        case 0x220D: io_.snv = static_cast<uint16_t>((io_.snv & 0x00FF) | v << 8); return;
        case 0x220E: io_.siv = static_cast<uint16_t>((io_.siv & 0xFF00) | v); return;
        case 0x220F: io_.siv = static_cast<uint16_t>((io_.siv & 0x00FF) | v << 8); return;
        case 0x2210:
            io_.hen = v & 1;
            io_.ven = v & 2;
            io_.hvselb = v & 0x80;
            return;
        case 0x2211: hcounter_ = vcounter_ = 0; return;
        case 0x2212: io_.hcnt = static_cast<uint16_t>((io_.hcnt & 0xFF00) | v); return;
        case 0x2213: io_.hcnt = static_cast<uint16_t>((io_.hcnt & 0x00FF) | v << 8); return;
        case 0x2214: io_.vcnt = static_cast<uint16_t>((io_.vcnt & 0xFF00) | v); return;
        case 0x2215: io_.vcnt = static_cast<uint16_t>((io_.vcnt & 0x00FF) | v << 8); return;
        case 0x2225:
            io_.cbm = v & 0x7F;
            io_.sw46 = v & 0x80;
            return;
        case 0x2227: io_.cwen = v & 0x80; return;
        case 0x222A: io_.ciwp = v; return;
        case 0x2230:  // DCNT
            io_.sd = v & 3;
            io_.dd = v & 4;
            io_.cdsel = v & 0x10;
            io_.cden = v & 0x20;
            io_.dprio = v & 0x40;
            io_.dmaen = v & 0x80;
            if (!io_.dmaen) dmaLine_ = 0;
            return;
        case 0x2231:
        case 0x2232:
        case 0x2233:
        case 0x2234:
        case 0x2235:
        case 0x2236:
        case 0x2237: ioWriteShared(address, v); return;
        case 0x2238: io_.dtc = static_cast<uint16_t>((io_.dtc & 0xFF00) | v); return;
        case 0x2239: io_.dtc = static_cast<uint16_t>((io_.dtc & 0x00FF) | v << 8); return;
        case 0x223F: io_.bbf = v & 0x80; return;
        case 0x2250:  // MCNT
            io_.md = v & 1;
            io_.acm = v & 2;
            if (io_.acm) io_.mr = 0;
            return;
        case 0x2251: io_.ma = static_cast<uint16_t>((io_.ma & 0xFF00) | v); return;
        case 0x2252: io_.ma = static_cast<uint16_t>((io_.ma & 0x00FF) | v << 8); return;
        case 0x2253: io_.mb = static_cast<uint16_t>((io_.mb & 0xFF00) | v); return;
        case 0x2254:
            io_.mb = static_cast<uint16_t>((io_.mb & 0x00FF) | v << 8);
            if (!io_.acm) {
                if (!io_.md) {  // signed multiplication
                    io_.mr = static_cast<uint32_t>(static_cast<int16_t>(io_.ma) * static_cast<int16_t>(io_.mb));
                    io_.mb = 0;
                } else {  // division: a signed dividend, an unsigned divisor
                    if (!io_.mb) {
                        io_.mr = 0;
                    } else {
                        const int32_t dividend = static_cast<int16_t>(io_.ma);
                        const int32_t divisor = io_.mb;
                        const int32_t rem = dividend >= 0 ? dividend % divisor : (dividend % divisor + divisor) % divisor;
                        const uint16_t quotient = static_cast<uint16_t>((dividend - rem) / divisor);
                        io_.mr = static_cast<uint64_t>(static_cast<uint16_t>(rem)) << 16 | quotient;
                    }
                    io_.ma = io_.mb = 0;
                }
            } else {  // the sum of products, 40 bits
                io_.mr += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(io_.ma) * static_cast<int16_t>(io_.mb)));
                io_.overflow = (io_.mr >> 40) & 1;
                io_.mr &= 0xFFFFFFFFFFull;
                io_.mb = 0;
            }
            return;
        case 0x2258:  // VBD
            io_.vb = v & 0x0F;
            io_.hl = v & 0x80;
            if (!io_.vb) io_.vb = 16;
            if (!io_.hl) {
                io_.vbit = static_cast<uint8_t>(io_.vbit + io_.vb);
                io_.va = (io_.va + (io_.vbit >> 3)) & 0xFFFFFF;
                io_.vbit &= 7;
            }
            return;
        case 0x2259: io_.va = (io_.va & 0xFFFF00) | v; return;
        case 0x225A: io_.va = (io_.va & 0xFF00FF) | static_cast<uint32_t>(v) << 8; return;
        case 0x225B:
            io_.va = (io_.va & 0x00FFFF) | static_cast<uint32_t>(v) << 16;
            io_.vbit = 0;
            return;
        default:
            if ((address & 0x1FF) >= 0x40 && (address & 0x1FF) <= 0x4F) {  // BRF
                const size_t i = (address & 0x1FF) - 0x40;
                io_.brf[i] = v;
                if ((i == 7 || i == 15) && io_.dmaen && io_.cden && !io_.cdsel) dmaCc2();
            }
            return;
    }
}

void Sa1::ioWriteShared(uint16_t address, uint8_t v) {
    switch (0x2200 | (address & 0x1FF)) {
        case 0x2231:  // CDMA
            io_.dmacb = std::min<uint8_t>(v & 3, 2);
            io_.dmasize = std::min<uint8_t>((v >> 2) & 7, 5);
            io_.chdend = v & 0x80;
            if (io_.chdend) bwramDma_ = false;
            return;
        case 0x2232: io_.dsa = (io_.dsa & 0xFFFF00) | v; return;
        case 0x2233: io_.dsa = (io_.dsa & 0xFF00FF) | static_cast<uint32_t>(v) << 8; return;
        case 0x2234: io_.dsa = (io_.dsa & 0x00FFFF) | static_cast<uint32_t>(v) << 16; return;
        case 0x2235: io_.dda = (io_.dda & 0xFFFF00) | v; return;
        case 0x2236:
            io_.dda = (io_.dda & 0xFF00FF) | static_cast<uint32_t>(v) << 8;
            if (io_.dmaen) {
                if (!io_.cden && !io_.dd) dmaNormal();
                else if (io_.cden && io_.cdsel) dmaCc1();
            }
            return;
        case 0x2237:
            io_.dda = (io_.dda & 0x00FFFF) | static_cast<uint32_t>(v) << 16;
            if (io_.dmaen && !io_.cden && io_.dd) dmaNormal();
            return;
        default: return;
    }
}

// ---- DMA: plain copies, and the character conversions.

void Sa1::dmaNormal() {
    while (io_.dtc--) {
        const uint32_t source = io_.dsa, target = io_.dda;
        io_.dsa = (io_.dsa + 1) & 0xFFFFFF;
        io_.dda = (io_.dda + 1) & 0xFFFFFF;
        uint8_t data = mdr_;
        if (io_.sd == 0 && io_.dd) {  // ROM to BW-RAM
            tick(bwramConflict() ? 8 : 4);
            data = romReadSa1(source);
            bwWrite(target, data);
        } else if (io_.sd == 0 && !io_.dd) {  // ROM to I-RAM
            tick(2 + ((iramConflict() || romConflict()) ? 2 : 0) + (iramConflict() ? 2 : 0));
            data = romReadSa1(source);
            iram_[target & 0x7FF] = data;
        } else if (io_.sd == 1 && !io_.dd) {  // BW-RAM to I-RAM
            tick(4 + ((bwramConflict() || iramConflict()) ? 2 : 0) + (bwramConflict() ? 2 : 0));
            data = bwRead(source);
            iram_[target & 0x7FF] = data;
        } else if (io_.sd == 2 && io_.dd) {  // I-RAM to BW-RAM
            tick(4 + ((bwramConflict() || iramConflict()) ? 2 : 0) + (bwramConflict() ? 2 : 0));
            data = iram_[source & 0x7FF];
            bwWrite(target, data);
        }
    }
    io_.dmaIrqfl = true;
    if (io_.dmaIrqen) io_.dmaIrqcl = false;
}

void Sa1::dmaCc1() {
    // From now on the console's processor reads the bitmap in BW-RAM as tiles.
    bwramDma_ = true;
    io_.chdmaIrqfl = true;
    if (io_.chdmaIrqen) io_.chdmaIrqcl = false;
}

uint8_t Sa1::dmaCc1Read(uint32_t address) {
    // 16 bytes a tile at 2 bits a pixel, 32 at 4, 64 at 8.
    const uint32_t charmask = (1u << (6 - io_.dmacb)) - 1;
    if ((address & charmask) == 0 && !bwram_.empty()) {
        // The next tile, converted into I-RAM.
        const uint32_t bpp = 2u << (2 - io_.dmacb);
        const uint32_t bpl = (8u << io_.dmasize) >> io_.dmacb;
        const uint32_t bwmask = static_cast<uint32_t>(bwram_.size()) - 1;
        const uint32_t tile = ((address - io_.dsa) & bwmask) >> (6 - io_.dmacb);
        const uint32_t ty = tile >> io_.dmasize, tx = tile & ((1u << io_.dmasize) - 1);
        uint32_t bwaddr = io_.dsa + ty * 8 * bpl + tx * bpp;
        for (uint32_t y = 0; y < 8; ++y) {
            uint64_t data = 0;
            for (uint32_t byte = 0; byte < bpp; ++byte)
                data |= static_cast<uint64_t>(bwRead((bwaddr + byte) & bwmask)) << (byte << 3);
            bwaddr += bpl;
            uint8_t out[8] = {};
            for (uint32_t x = 0; x < 8; ++x) {
                for (uint32_t plane = 0; plane < bpp; ++plane) {
                    out[plane] = static_cast<uint8_t>(out[plane] | (data & 1) << (7 - x));
                    data >>= 1;
                }
            }
            for (uint32_t byte = 0; byte < bpp; ++byte) {
                const uint32_t p = io_.dda + (y << 1) + ((byte & 6) << 3) + (byte & 1);
                iram_[p & 0x7FF] = out[byte];
            }
        }
    }
    return iram_[(io_.dda + (address & charmask)) & 0x7FF];
}

void Sa1::dmaCc2() {
    const uint8_t* brf = &io_.brf[static_cast<size_t>((dmaLine_ & 1) << 3)];
    const uint32_t bpp = 2u << (2 - io_.dmacb);
    uint32_t address = io_.dda & 0x7FF;
    address &= ~((1u << (7 - io_.dmacb)) - 1);
    address += (dmaLine_ & 8u) * bpp;
    address += (dmaLine_ & 7u) * 2;
    for (uint32_t byte = 0; byte < bpp; ++byte) {
        uint8_t output = 0;
        for (uint32_t bit = 0; bit < 8; ++bit) output = static_cast<uint8_t>(output | ((brf[bit] >> byte) & 1) << (7 - bit));
        iram_[(address + ((byte & 6) << 3) + (byte & 1)) & 0x7FF] = output;
    }
    dmaLine_ = static_cast<uint8_t>((dmaLine_ + 1) & 15);
}

}  // namespace plumnin::snes
