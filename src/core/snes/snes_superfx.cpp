#include "snes_superfx.h"

namespace plumnin::snes {

namespace {

uint32_t roundUpPow2(size_t n) {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

SuperFx::SuperFx(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram) : rom_(rom), ram_(ram) {
    // A ROM whose size is not a power of two repeats (the voxel demo's is one).
    romMask_ = roundUpPow2(rom_.size()) - 1;
    if (ram_.empty()) ram_.assign(0x8000, 0);
    ramMask_ = roundUpPow2(ram_.size()) - 1;
}

void SuperFx::power() { reset(); }

void SuperFx::reset() {
    r_.fill(0);
    mod14_ = mod15_ = false;
    z_ = cy_ = s_ = ov_ = g_ = sfrR_ = false;
    alt1_ = alt2_ = il_ = ih_ = b_ = irq_ = false;
    pbr_ = rombr_ = 0;
    rambr_ = false;
    cbr_ = 0;
    scbr_ = scmr_ = colr_ = por_ = 0;
    bramr_ = false;
    cfgr_ = 0;
    clsr_ = false;
    pipeline_ = 0x01;  // nop
    ramaddr_ = 0;
    romcl_ = ramcl_ = 0;
    romdr_ = ramdr_ = 0;
    ramar_ = 0;
    sreg_ = dreg_ = 0;
    cache_.fill(0);
    cacheValid_.fill(false);
    pixels_ = {};
    budget_ = 0;
}

uint16_t SuperFx::sfr() const {
    return static_cast<uint16_t>(z_ << 1 | cy_ << 2 | s_ << 3 | ov_ << 4 | g_ << 5 | sfrR_ << 6 | alt1_ << 8 | alt2_ << 9 |
                                 il_ << 10 | ih_ << 11 | b_ << 12 | irq_ << 15);
}

void SuperFx::setSfr(uint16_t v) {
    z_ = v & 0x0002;
    cy_ = v & 0x0004;
    s_ = v & 0x0008;
    ov_ = v & 0x0010;
    g_ = v & 0x0020;
    sfrR_ = v & 0x0040;
    alt1_ = v & 0x0100;
    alt2_ = v & 0x0200;
    il_ = v & 0x0400;
    ih_ = v & 0x0800;
    b_ = v & 0x1000;
    irq_ = v & 0x8000;
}

// ---- The GSU's bus: the ROM at 00-5F, the RAM at 70-71.

uint8_t SuperFx::busRead(uint32_t address) {
    if ((address & 0xC00000) == 0x000000)
        return rom_.empty() ? 0 : rom_[((((address & 0x3F0000) >> 1) | (address & 0x7FFF)) & romMask_) % rom_.size()];
    if ((address & 0xE00000) == 0x400000) return rom_.empty() ? 0 : rom_[(address & romMask_) % rom_.size()];
    if ((address & 0xFE0000) == 0x700000) return ram_[(address & ramMask_) % ram_.size()];
    return 0;
}

void SuperFx::busWrite(uint32_t address, uint8_t value) {
    if ((address & 0xFE0000) == 0x700000) ram_[(address & ramMask_) % ram_.size()] = value;
}

// ---- Time: the buffered ROM read and RAM write finish while it goes on.

void SuperFx::step(int clocks) {
    budget_ -= clocks;
    if (romcl_) {
        romcl_ -= std::min(clocks, romcl_);
        if (!romcl_) {
            sfrR_ = false;
            romdr_ = busRead((static_cast<uint32_t>(rombr_) << 16) + r_[14]);
        }
    }
    if (ramcl_) {
        ramcl_ -= std::min(clocks, ramcl_);
        if (!ramcl_) busWrite(0x700000u + (static_cast<uint32_t>(rambr_) << 16) + ramar_, ramdr_);
    }
}

void SuperFx::catchUp() {
    if (!g_) {
        // Stopped: nothing to catch up with.
        budget_ = 0;
        return;
    }
    while (budget_ > 0 && g_) execute();
    if (!g_) budget_ = 0;
}

uint8_t SuperFx::readOpcode(uint16_t address) {
    const uint16_t offset = static_cast<uint16_t>(address - cbr_);
    if (offset < 512) {
        if (!cacheValid_[offset >> 4]) {
            // The cache fills sixteen bytes at a time.
            uint32_t dp = offset & 0xFFF0;
            uint32_t from = static_cast<uint32_t>(pbr_) << 16 | ((cbr_ + dp) & 0xFFF0);
            for (int n = 0; n < 16; ++n) {
                step(fast(5, 6));
                cache_[dp++ & 511] = busRead(from++);
            }
            cacheValid_[offset >> 4] = true;
        } else {
            step(fast(1, 2));
        }
        return cache_[offset];
    }
    if (pbr_ <= 0x5F) syncRom();
    else syncRam();
    step(fast(5, 6));
    return busRead(static_cast<uint32_t>(pbr_) << 16 | address);
}

uint8_t SuperFx::pipe() {
    const uint8_t result = pipeline_;
    r_[15] = static_cast<uint16_t>(r_[15] + 1);
    pipeline_ = readOpcode(r_[15]);
    mod15_ = false;
    return result;
}

void SuperFx::execute() {
    const uint8_t op = pipeline_;
    pipeline_ = readOpcode(r_[15]);
    mod15_ = false;
    instruction(op);
    if (mod14_) {
        mod14_ = false;
        updateRomBuffer();
    }
    if (mod15_) mod15_ = false;
    else r_[15] = static_cast<uint16_t>(r_[15] + 1);
}

// ---- PLOT and RPIX: pixels into the RAM's tiles, eight at a time.

uint8_t SuperFx::color(uint8_t source) const {
    if (por_ & 0x04) return static_cast<uint8_t>((colr_ & 0xF0) | (source >> 4));  // high nibble
    if (por_ & 0x08) return static_cast<uint8_t>((colr_ & 0xF0) | (source & 0x0F));  // freeze high
    return source;
}

uint32_t SuperFx::tileAddress(uint8_t x, uint8_t y) const {
    const int ht = (scmr_ & 0x20 ? 2 : 0) | (scmr_ & 0x04 ? 1 : 0);
    uint32_t cn = 0;  // the tile's number
    switch (por_ & 0x10 ? 3 : ht) {
        case 0: cn = ((x & 0xF8u) << 1) + ((y & 0xF8u) >> 3); break;                       // 128 lines
        case 1: cn = ((x & 0xF8u) << 1) + ((x & 0xF8u) >> 1) + ((y & 0xF8u) >> 3); break;  // 160
        case 2: cn = ((x & 0xF8u) << 1) + (x & 0xF8u) + ((y & 0xF8u) >> 3); break;          // 192
        default:                                                                           // sprites
            cn = ((y & 0x80u) << 2) + ((x & 0x80u) << 1) + ((y & 0x78u) << 1) + ((x & 0x78u) >> 3);
            break;
    }
    const int md = scmr_ & 3;
    const uint32_t bpp = 2u << (md - (md >> 1));  // 2, 4, 4, 8
    return 0x700000u + cn * (bpp << 3) + (static_cast<uint32_t>(scbr_) << 10) + (y & 7u) * 2;
}

void SuperFx::plot(uint8_t x, uint8_t y) {
    const int md = scmr_ & 3;
    if (!(por_ & 0x01)) {  // colour 0 is transparent unless told otherwise
        if (md == 3 && !(por_ & 0x08)) {
            if (colr_ == 0) return;
        } else if ((colr_ & 0x0F) == 0) {
            return;
        }
    }
    uint8_t c = colr_;
    if ((por_ & 0x02) && md != 3) {  // dither
        if ((x ^ y) & 1) c >>= 4;
        c &= 0x0F;
    }
    const uint16_t offset = static_cast<uint16_t>((y << 5) + (x >> 3));
    if (offset != pixels_[0].offset) {
        flushPixels(pixels_[1]);
        pixels_[1] = pixels_[0];
        pixels_[0].bitpend = 0;
        pixels_[0].offset = offset;
    }
    const int bit = (x & 7) ^ 7;
    pixels_[0].data[static_cast<size_t>(bit)] = c;
    pixels_[0].bitpend |= static_cast<uint8_t>(1 << bit);
    if (pixels_[0].bitpend == 0xFF) {
        flushPixels(pixels_[1]);
        pixels_[1] = pixels_[0];
        pixels_[0].bitpend = 0;
    }
}

uint8_t SuperFx::rpix(uint8_t x, uint8_t y) {
    flushPixels(pixels_[1]);
    flushPixels(pixels_[0]);
    const int md = scmr_ & 3;
    const uint32_t bpp = 2u << (md - (md >> 1));
    const uint32_t addr = tileAddress(x, y);
    const int bit = (x & 7) ^ 7;
    uint8_t data = 0;
    for (uint32_t n = 0; n < bpp; ++n) {
        const uint32_t byte = ((n >> 1) << 4) + (n & 1);
        step(fast(5, 6));
        data |= static_cast<uint8_t>(((busRead(addr + byte) >> bit) & 1) << n);
    }
    return data;
}

void SuperFx::flushPixels(PixelCache& c) {
    if (!c.bitpend) return;
    const uint8_t x = static_cast<uint8_t>(c.offset << 3), y = static_cast<uint8_t>(c.offset >> 5);
    const int md = scmr_ & 3;
    const uint32_t bpp = 2u << (md - (md >> 1));
    const uint32_t addr = tileAddress(x, y);
    for (uint32_t n = 0; n < bpp; ++n) {
        const uint32_t byte = ((n >> 1) << 4) + (n & 1);
        uint8_t data = 0;
        for (int i = 0; i < 8; ++i) data |= static_cast<uint8_t>(((c.data[static_cast<size_t>(i)] >> n) & 1) << i);
        if (c.bitpend != 0xFF) {
            step(fast(5, 6));
            data = static_cast<uint8_t>((data & c.bitpend) | (busRead(addr + byte) & ~c.bitpend));
        }
        step(fast(5, 6));
        busWrite(addr + byte, data);
    }
    c.bitpend = 0;
}

// ---- The instructions. ALT1/ALT2 (and WITH's B flag) change what the next one does.

void SuperFx::instruction(uint8_t op) {
    const int n = op & 15;
    const int alt = (alt2_ ? 2 : 0) | (alt1_ ? 1 : 0);
    auto branch = [&](bool take) {
        const int8_t d = static_cast<int8_t>(pipe());
        if (take) set(15, static_cast<uint16_t>(r_[15] + d));
    };
    switch (op >> 4) {
        case 0x0:
            switch (op) {
                case 0x00:  // stop
                    if (!(cfgr_ & 0x80)) irq_ = true;
                    g_ = false;
                    pipeline_ = 0x01;
                    resetPrefix();
                    return;
                case 0x01: resetPrefix(); return;  // nop
                case 0x02:                         // cache
                    if (cbr_ != (r_[15] & 0xFFF0)) {
                        cbr_ = r_[15] & 0xFFF0;
                        flushCache();
                    }
                    resetPrefix();
                    return;
                case 0x03: {  // lsr
                    const uint16_t v = sr();
                    cy_ = v & 1;
                    setDr(static_cast<uint16_t>(v >> 1));
                    setSz(r_[dreg_]);
                    resetPrefix();
                    return;
                }
                case 0x04: {  // rol
                    const uint16_t v = sr();
                    setDr(static_cast<uint16_t>(v << 1 | (cy_ ? 1 : 0)));
                    cy_ = v & 0x8000;
                    setSz(r_[dreg_]);
                    resetPrefix();
                    return;
                }
                case 0x05: branch(true); return;
                case 0x06: branch(s_ == ov_); return;  // bge
                case 0x07: branch(s_ != ov_); return;  // blt
                case 0x08: branch(!z_); return;
                case 0x09: branch(z_); return;
                case 0x0A: branch(!s_); return;
                case 0x0B: branch(s_); return;
                case 0x0C: branch(!cy_); return;
                case 0x0D: branch(cy_); return;
                case 0x0E: branch(!ov_); return;
                case 0x0F: branch(ov_); return;
            }
            return;
        case 0x1:  // to rN / move rN
            if (!b_) {
                dreg_ = static_cast<uint8_t>(n);
            } else {
                set(n, sr());
                resetPrefix();
            }
            return;
        case 0x2:  // with rN
            sreg_ = dreg_ = static_cast<uint8_t>(n);
            b_ = true;
            return;
        case 0x3:
            if (n < 12) {  // stw (rN) / stb (rN)
                ramaddr_ = r_[n];
                writeRamBuffer(ramaddr_, static_cast<uint8_t>(sr()));
                if (!alt1_) writeRamBuffer(ramaddr_ ^ 1, static_cast<uint8_t>(sr() >> 8));
                resetPrefix();
            } else if (n == 12) {  // loop
                set(12, static_cast<uint16_t>(r_[12] - 1));
                setSz(r_[12]);
                if (!z_) set(15, r_[13]);
                resetPrefix();
            } else if (n == 13) {
                b_ = false;
                alt1_ = true;
            } else if (n == 14) {
                b_ = false;
                alt2_ = true;
            } else {
                b_ = false;
                alt1_ = alt2_ = true;
            }
            return;
        case 0x4:
            if (n < 12) {  // ldw (rN) / ldb (rN)
                ramaddr_ = r_[n];
                uint16_t v = readRamBuffer(ramaddr_);
                if (!alt1_) v = static_cast<uint16_t>(v | readRamBuffer(ramaddr_ ^ 1) << 8);
                setDr(v);
                resetPrefix();
                return;
            }
            switch (n) {
                case 12:
                    if (!alt1_) {  // plot
                        plot(static_cast<uint8_t>(r_[1]), static_cast<uint8_t>(r_[2]));
                        set(1, static_cast<uint16_t>(r_[1] + 1));
                    } else {  // rpix
                        setDr(rpix(static_cast<uint8_t>(r_[1]), static_cast<uint8_t>(r_[2])));
                        setSz(r_[dreg_]);
                    }
                    break;
                case 13:  // swap
                    setDr(static_cast<uint16_t>(sr() >> 8 | sr() << 8));
                    setSz(r_[dreg_]);
                    break;
                case 14:  // color / cmode
                    if (!alt1_) colr_ = color(static_cast<uint8_t>(sr()));
                    else por_ = static_cast<uint8_t>(sr() & 0x1F);
                    break;
                case 15:  // not
                    setDr(static_cast<uint16_t>(~sr()));
                    setSz(r_[dreg_]);
                    break;
            }
            resetPrefix();
            return;
        case 0x5: {  // add / adc, rN or #N
            const uint32_t v = alt2_ ? static_cast<uint32_t>(n) : r_[n];
            const uint32_t a = sr();
            const uint32_t res = a + v + (alt1_ && cy_ ? 1 : 0);
            ov_ = ~(a ^ v) & (v ^ res) & 0x8000;
            s_ = res & 0x8000;
            cy_ = res >= 0x10000;
            z_ = (res & 0xFFFF) == 0;
            setDr(static_cast<uint16_t>(res));
            resetPrefix();
            return;
        }
        case 0x6: {  // sub / sbc / sub #N / cmp
            const uint32_t v = (!alt2_ || alt1_) ? r_[n] : static_cast<uint32_t>(n);
            const int32_t a = sr();
            const int32_t res = a - static_cast<int32_t>(v) - (alt == 1 && !cy_ ? 1 : 0);
            ov_ = (static_cast<uint32_t>(a) ^ v) & (static_cast<uint32_t>(a) ^ static_cast<uint32_t>(res)) & 0x8000;
            s_ = res & 0x8000;
            cy_ = res >= 0;
            z_ = (res & 0xFFFF) == 0;
            if (alt != 3) setDr(static_cast<uint16_t>(res));
            resetPrefix();
            return;
        }
        case 0x7:
            if (n == 0) {  // merge
                setDr(static_cast<uint16_t>((r_[7] & 0xFF00) | (r_[8] >> 8)));
                const uint16_t v = r_[dreg_];
                ov_ = v & 0xC0C0;
                s_ = v & 0x8080;
                cy_ = v & 0xE0E0;
                z_ = v & 0xF0F0;
            } else {  // and / bic, rN or #N
                const uint16_t v = alt2_ ? static_cast<uint16_t>(n) : r_[n];
                setDr(static_cast<uint16_t>(sr() & (alt1_ ? ~v : v)));
                setSz(r_[dreg_]);
            }
            resetPrefix();
            return;
        case 0x8: {  // mult / umult, rN or #N
            const uint16_t v = alt2_ ? static_cast<uint16_t>(n) : r_[n];
            const uint16_t res = !alt1_ ? static_cast<uint16_t>(static_cast<int8_t>(sr()) * static_cast<int8_t>(v))
                                        : static_cast<uint16_t>(static_cast<uint8_t>(sr()) * static_cast<uint8_t>(v));
            setDr(res);
            setSz(res);
            resetPrefix();
            if (!(cfgr_ & 0x20)) step(fast(1, 2));
            return;
        }
        case 0x9:
            if (n == 0) {  // sbk
                writeRamBuffer(ramaddr_, static_cast<uint8_t>(sr()));
                writeRamBuffer(ramaddr_ ^ 1, static_cast<uint8_t>(sr() >> 8));
            } else if (n <= 4) {  // link #N
                set(11, static_cast<uint16_t>(r_[15] + n));
            } else if (n == 5) {  // sex
                setDr(static_cast<uint16_t>(static_cast<int8_t>(sr())));
                setSz(r_[dreg_]);
            } else if (n == 6) {  // asr / div2
                const uint16_t v = sr();
                cy_ = v & 1;
                uint16_t res = static_cast<uint16_t>(static_cast<int16_t>(v) >> 1);
                if (alt1_ && v == 0xFFFF) res = 0;  // div2 of -1 is 0
                setDr(res);
                setSz(res);
            } else if (n == 7) {  // ror
                const uint16_t v = sr();
                setDr(static_cast<uint16_t>((cy_ ? 0x8000 : 0) | v >> 1));
                cy_ = v & 1;
                setSz(r_[dreg_]);
            } else if (n <= 13) {  // jmp rN / ljmp rN
                if (!alt1_) {
                    set(15, r_[n]);
                } else {
                    pbr_ = r_[n] & 0x7F;
                    set(15, sr());
                    cbr_ = r_[15] & 0xFFF0;
                    flushCache();
                }
            } else if (n == 14) {  // lob
                setDr(sr() & 0xFF);
                s_ = r_[dreg_] & 0x80;
                z_ = r_[dreg_] == 0;
            } else {  // fmult / lmult
                const uint32_t res = static_cast<uint32_t>(static_cast<int16_t>(sr()) * static_cast<int16_t>(r_[6]));
                if (alt1_) set(4, static_cast<uint16_t>(res));
                setDr(static_cast<uint16_t>(res >> 16));
                s_ = r_[dreg_] & 0x8000;
                cy_ = res & 0x8000;
                z_ = r_[dreg_] == 0;
                resetPrefix();
                step((cfgr_ & 0x20 ? 3 : 7) * fast(1, 2));
                return;
            }
            resetPrefix();
            return;
        case 0xA:
            if (alt1_) {  // lms rN, (yy)
                ramaddr_ = static_cast<uint16_t>(pipe() << 1);
                const uint8_t lo = readRamBuffer(ramaddr_);
                set(n, static_cast<uint16_t>(readRamBuffer(ramaddr_ ^ 1) << 8 | lo));
            } else if (alt2_) {  // sms (yy), rN
                ramaddr_ = static_cast<uint16_t>(pipe() << 1);
                writeRamBuffer(ramaddr_, static_cast<uint8_t>(r_[n]));
                writeRamBuffer(ramaddr_ ^ 1, static_cast<uint8_t>(r_[n] >> 8));
            } else {  // ibt rN, #pp
                set(n, static_cast<uint16_t>(static_cast<int8_t>(pipe())));
            }
            resetPrefix();
            return;
        case 0xB:  // from rN / moves rN
            if (!b_) {
                sreg_ = static_cast<uint8_t>(n);
            } else {
                setDr(r_[n]);
                const uint16_t v = r_[dreg_];
                ov_ = v & 0x80;
                setSz(v);
                resetPrefix();
            }
            return;
        case 0xC:
            if (n == 0) {  // hib
                setDr(sr() >> 8);
                s_ = r_[dreg_] & 0x80;
                z_ = r_[dreg_] == 0;
            } else {  // or / xor, rN or #N
                const uint16_t v = alt2_ ? static_cast<uint16_t>(n) : r_[n];
                setDr(!alt1_ ? static_cast<uint16_t>(sr() | v) : static_cast<uint16_t>(sr() ^ v));
                setSz(r_[dreg_]);
            }
            resetPrefix();
            return;
        case 0xD:
            if (n < 15) {  // inc rN
                set(n, static_cast<uint16_t>(r_[n] + 1));
                setSz(r_[n]);
            } else if (!alt2_) {  // getc
                colr_ = color(readRomBuffer());
            } else if (!alt1_) {  // ramb
                syncRam();
                rambr_ = sr() & 1;
            } else {  // romb
                syncRom();
                rombr_ = sr() & 0x7F;
            }
            resetPrefix();
            return;
        case 0xE:
            if (n < 15) {  // dec rN
                set(n, static_cast<uint16_t>(r_[n] - 1));
                setSz(r_[n]);
            } else {  // getb, getbh, getbl, getbs
                const uint8_t b = readRomBuffer();
                switch (alt) {
                    case 0: setDr(b); break;
                    case 1: setDr(static_cast<uint16_t>(b << 8 | (sr() & 0xFF))); break;
                    case 2: setDr(static_cast<uint16_t>((sr() & 0xFF00) | b)); break;
                    default: setDr(static_cast<uint16_t>(static_cast<int8_t>(b))); break;
                }
            }
            resetPrefix();
            return;
        case 0xF:
            if (alt1_) {  // lm rN, (xx)
                ramaddr_ = pipe();
                ramaddr_ = static_cast<uint16_t>(ramaddr_ | pipe() << 8);
                const uint8_t lo = readRamBuffer(ramaddr_);
                set(n, static_cast<uint16_t>(readRamBuffer(ramaddr_ ^ 1) << 8 | lo));
            } else if (alt2_) {  // sm (xx), rN
                ramaddr_ = pipe();
                ramaddr_ = static_cast<uint16_t>(ramaddr_ | pipe() << 8);
                writeRamBuffer(ramaddr_, static_cast<uint8_t>(r_[n]));
                writeRamBuffer(ramaddr_ ^ 1, static_cast<uint8_t>(r_[n] >> 8));
            } else {  // iwt rN, #xxxx
                const uint8_t lo = pipe();
                set(n, static_cast<uint16_t>(pipe() << 8 | lo));
            }
            resetPrefix();
            return;
    }
}

// ---- The console's side: the registers at 3000-34FF, the ROM and the RAM.

uint8_t SuperFx::ioRead(uint16_t address) {
    address = static_cast<uint16_t>(0x3000 | (address & 0x3FF));
    if (address >= 0x3100 && address <= 0x32FF) return cache_[(address - 0x3100 + cbr_) & 511];
    if (address <= 0x301F) return static_cast<uint8_t>(r_[(address >> 1) & 15] >> ((address & 1) * 8));
    switch (address) {
        case 0x3030: return static_cast<uint8_t>(sfr());
        case 0x3031: {
            const uint8_t v = static_cast<uint8_t>(sfr() >> 8);
            irq_ = false;
            return v;
        }
        case 0x3034: return pbr_;
        case 0x3036: return rombr_;
        case 0x303B: return 0x04;  // the version
        case 0x303C: return rambr_;
        case 0x303E: return static_cast<uint8_t>(cbr_);
        case 0x303F: return static_cast<uint8_t>(cbr_ >> 8);
        default: return 0;
    }
}

void SuperFx::ioWrite(uint16_t address, uint8_t value) {
    address = static_cast<uint16_t>(0x3000 | (address & 0x3FF));
    if (address >= 0x3100 && address <= 0x32FF) {
        const uint16_t a = static_cast<uint16_t>((address - 0x3100 + cbr_) & 511);
        cache_[a] = value;
        if ((a & 15) == 15) cacheValid_[a >> 4] = true;
        return;
    }
    if (address <= 0x301F) {
        const int n = (address >> 1) & 15;
        if (!(address & 1)) r_[static_cast<size_t>(n)] = static_cast<uint16_t>((r_[static_cast<size_t>(n)] & 0xFF00) | value);
        else r_[static_cast<size_t>(n)] = static_cast<uint16_t>(value << 8 | (r_[static_cast<size_t>(n)] & 0xFF));
        if (n == 14) updateRomBuffer();
        if (address == 0x301F) {
            g_ = true;  // writing R15's high byte starts it
            budget_ = 0;
        }
        return;
    }
    switch (address) {
        case 0x3030: {
            const bool was = g_;
            setSfr(static_cast<uint16_t>((sfr() & 0xFF00) | value));
            if (was && !g_) {
                cbr_ = 0;
                flushCache();
            }
            if (!was && g_) budget_ = 0;
            break;
        }
        case 0x3031: setSfr(static_cast<uint16_t>(value << 8 | (sfr() & 0x00FF))); break;
        case 0x3033: bramr_ = value & 1; break;
        case 0x3034:
            pbr_ = value & 0x7F;
            flushCache();
            break;
        case 0x3037: cfgr_ = value & 0xA0; break;
        case 0x3038: scbr_ = value; break;
        case 0x3039: clsr_ = value & 1; break;
        case 0x303A: scmr_ = value & 0x3F; break;
        default: break;
    }
}

namespace {

// The interrupt vectors the processor reads while the GSU has the ROM: into WRAM, where
// the games keep their handlers for that time.
constexpr uint8_t kVectors[16] = {0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01,
                                  0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0C, 0x01};

}  // namespace

bool SuperFx::read(uint32_t address, uint8_t& value) {
    const uint32_t bank = (address >> 16) & 0xFF, a = address & 0xFFFF;
    const uint32_t low = bank & 0x7F;
    if (low < 0x40 && a >= 0x3000 && a < 0x3500) {
        catchUp();
        value = ioRead(static_cast<uint16_t>(a));
        return true;
    }
    const bool romBusy = g_ && (scmr_ & 0x10), ramBusy = g_ && (scmr_ & 0x08);
    if ((low < 0x40 && a >= 0x8000) || (low >= 0x40 && low < 0x60)) {
        if (romBusy) {
            value = kVectors[a & 15];
            return true;
        }
        const uint32_t o = low < 0x40 ? ((low << 15) | (a & 0x7FFF)) : (((low - 0x40) << 16) | a);
        value = rom_.empty() ? 0 : rom_[(o & romMask_) % rom_.size()];
        return true;
    }
    int64_t r = -1;
    if (low < 0x40 && a >= 0x6000 && a < 0x8000) r = a - 0x6000;
    else if (low >= 0x70 && low < 0x72) r = ((low & 1) << 16) | a;
    if (r >= 0) {
        if (ramBusy) return false;  // open bus
        value = ram_[(static_cast<uint32_t>(r) & ramMask_) % ram_.size()];
        return true;
    }
    return false;
}

bool SuperFx::write(uint32_t address, uint8_t value) {
    const uint32_t bank = (address >> 16) & 0xFF, a = address & 0xFFFF;
    const uint32_t low = bank & 0x7F;
    if (low < 0x40 && a >= 0x3000 && a < 0x3500) {
        catchUp();
        ioWrite(static_cast<uint16_t>(a), value);
        return true;
    }
    int64_t r = -1;
    if (low < 0x40 && a >= 0x6000 && a < 0x8000) r = a - 0x6000;
    else if (low >= 0x70 && low < 0x72) r = ((low & 1) << 16) | a;
    if (r >= 0) {
        uint8_t& b = ram_[(static_cast<uint32_t>(r) & ramMask_) % ram_.size()];
        if (b != value) batteryChanged_ = true;
        b = value;
        return true;
    }
    return (low < 0x40 && a >= 0x8000) || (low >= 0x40 && low < 0x60);  // the ROM: nothing to write
}

}  // namespace plumnin::snes
