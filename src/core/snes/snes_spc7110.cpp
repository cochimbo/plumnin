#include "snes_spc7110.h"

#include <algorithm>

namespace plumnin::snes {

// Checked against ares' SPC7110.

namespace {

struct Model {
    uint8_t probability;  // of the likelier symbol
    uint8_t next[2];      // the next state after it, or after the other
};
constexpr Model kEvolution[53] = {
    {0x5a, {1, 1}},   {0x25, {2, 6}},   {0x11, {3, 8}},   {0x08, {4, 10}},  {0x03, {5, 12}},  {0x01, {5, 15}},
    {0x5a, {7, 7}},   {0x3f, {8, 19}},  {0x2c, {9, 21}},  {0x20, {10, 22}}, {0x17, {11, 23}}, {0x11, {12, 25}},
    {0x0c, {13, 26}}, {0x09, {14, 28}}, {0x07, {15, 29}}, {0x05, {16, 31}}, {0x04, {17, 32}}, {0x03, {18, 34}},
    {0x02, {5, 35}},  {0x5a, {20, 20}}, {0x48, {21, 39}}, {0x3a, {22, 40}}, {0x2e, {23, 42}}, {0x26, {24, 44}},
    {0x1f, {25, 45}}, {0x19, {26, 46}}, {0x15, {27, 25}}, {0x11, {28, 26}}, {0x0e, {29, 26}}, {0x0b, {30, 27}},
    {0x09, {31, 28}}, {0x08, {32, 29}}, {0x07, {33, 30}}, {0x05, {34, 31}}, {0x04, {35, 33}}, {0x04, {36, 33}},
    {0x03, {37, 34}}, {0x02, {38, 35}}, {0x02, {5, 36}},  {0x58, {40, 39}}, {0x4d, {41, 47}}, {0x43, {42, 48}},
    {0x3b, {43, 49}}, {0x34, {44, 50}}, {0x2e, {45, 51}}, {0x29, {46, 44}}, {0x25, {24, 45}}, {0x56, {48, 47}},
    {0x4f, {49, 47}}, {0x47, {50, 48}}, {0x41, {51, 49}}, {0x3c, {52, 50}}, {0x37, {43, 51}},
};
constexpr uint32_t kMps = 0, kLps = 1, kHalf = 0x55, kMax = 0xFF;

// The inverse Morton transform: packed pixels to planes (odd bits low, even bits high).
uint32_t deinterleave(uint64_t data, uint32_t bits) {
    data &= bits >= 64 ? ~0ull : (1ull << bits) - 1;
    data = 0x5555555555555555ull & (data << bits | data >> 1);
    data = 0x3333333333333333ull & (data | data >> 1);
    data = 0x0f0f0f0f0f0f0f0full & (data | data >> 2);
    data = 0x00ff00ff00ff00ffull & (data | data >> 4);
    data = 0x0000ffff0000ffffull & (data | data >> 8);
    return static_cast<uint32_t>(data | data >> 16);
}

// That colour to the front of the list of sixteen.
uint64_t moveToFront(uint64_t list, uint32_t nibble) {
    uint64_t mask = ~15ull;
    for (uint32_t n = 0; n < 64; n += 4, mask <<= 4) {
        if ((list >> n & 15) != nibble) continue;
        return (list & mask) + (list << 4 & ~mask) + nibble;
    }
    return list;
}

}  // namespace

void Spc7110::power() {
    r_.fill(0);
    r_[0x32] = 0x01;
    r_[0x33] = 0x02;
    dcuMode_ = 0;
    dcuAddress_ = dcuOffset_ = 0;
    dcuTile_.fill(0);
    ctxPrediction_.fill(0);
    ctxSwap_.fill(0);
    dBpp_ = 1;
}

// The bus.

uint8_t Spc7110::programRead(uint32_t offset) const {
    if (rom_.empty()) return 0;
    const uint32_t size = static_cast<uint32_t>(std::min<size_t>(rom_.size(), kProgramSize));
    return rom_[offset % size];
}

uint8_t Spc7110::dataRead(uint32_t address) const {
    if (rom_.size() <= kProgramSize) return 0;
    const uint32_t sizeMb = 1u << (r_[0x34] & 3);
    if ((r_[0x34] & 3) != 3 && (address & 0x400000)) return 0;
    const uint32_t offset = address & (0x100000 * sizeMb - 1) & 0xFFFFFF;
    return rom_[kProgramSize + offset % (rom_.size() - kProgramSize)];
}

// 00-3F:8000-FFFF mirror the upper halves of C0-FF: four 1 MB banks, the first the
// program (two with 4834 bit 2), the others the data ROM's megabytes from 4830-4833.
uint8_t Spc7110::mcuRomRead(uint32_t address) const {
    const uint32_t a = address & 0x0FFFFF;
    switch ((address >> 20) & 3) {
        case 0: return programRead(a);
        case 1:
            if (r_[0x34] & 4) return programRead(0x100000 + a);
            return dataRead(a | 0x100000 * (r_[0x31] & 7));
        case 2: return dataRead(a | 0x100000 * (r_[0x32] & 7));
        default: return dataRead(a | 0x100000 * (r_[0x33] & 7));
    }
}

bool Spc7110::read(uint32_t address, uint8_t& value) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if (bank == 0x50 || bank == 0x58) {
        value = ioRead(bank == 0x50 ? 0x00 : 0x08);
        return true;
    }
    if (bank >= 0xC0) {
        value = mcuRomRead(address & 0x3FFFFF);
        return true;
    }
    if ((bank & 0x7F) >= 0x40) return false;
    if (a >= 0x4800 && a < 0x4840) {
        value = ioRead(a & 0x3F);
        return true;
    }
    if (a >= 0x6000 && a < 0x8000) {
        value = (r_[0x30] & 0x80) && !ram_.empty() ? ram_[((bank & 0x3F) << 13 | (a & 0x1FFF)) % ram_.size()] : 0;
        return true;
    }
    if (a < 0x8000) return false;
    value = mcuRomRead((bank & 0x3F) << 16 | a);
    return true;
}

bool Spc7110::write(uint32_t address, uint8_t value) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if (bank == 0x50 || bank == 0x58) {
        ioWrite(bank == 0x50 ? 0x00 : 0x08, value);
        return true;
    }
    if (bank >= 0xC0) return true;
    if ((bank & 0x7F) >= 0x40) return false;
    if (a >= 0x4800 && a < 0x4840) {
        ioWrite(a & 0x3F, value);
        return true;
    }
    if (a >= 0x6000 && a < 0x8000) {
        if ((r_[0x30] & 0x80) && !ram_.empty()) {
            uint8_t& b = ram_[((bank & 0x3F) << 13 | (a & 0x1FFF)) % ram_.size()];
            if (b != value) batteryChanged_ = true;
            b = value;
        }
        return true;
    }
    return a >= 0x8000;
}

uint8_t Spc7110::ioRead(uint8_t reg) {
    switch (reg) {
        case 0x00: {  // the next decompressed byte; the counter goes down
            const uint16_t counter = static_cast<uint16_t>((r_[0x09] | r_[0x0A] << 8) - 1);
            r_[0x09] = static_cast<uint8_t>(counter);
            r_[0x0A] = static_cast<uint8_t>(counter >> 8);
            return dcuRead();
        }
        case 0x08: return 0;
        case 0x10: {  // the data port
            const uint8_t v = r_[0x10];
            dataPortIncrement();
            return v;
        }
        case 0x1A:
            dataPortSeek(3);
            return 0;
        default: break;
    }
    if (reg <= 0x0C || (reg >= 0x11 && reg <= 0x18) || (reg >= 0x20 && reg <= 0x2F) || (reg >= 0x30 && reg <= 0x34))
        return r_[reg];
    return 0;
}

void Spc7110::ioWrite(uint8_t reg, uint8_t value) {
    switch (reg) {
        case 0x01: case 0x02: case 0x05: case 0x07: case 0x09: case 0x0A: r_[reg] = value; break;
        case 0x03: r_[reg] = value & 0x7F; break;
        case 0x04:
            r_[reg] = value;
            dcuLoadAddress();
            break;
        case 0x06:  // starts the decompression
            r_[reg] = value;
            r_[0x0C] &= 0x7F;
            dcuBeginTransfer();
            break;
        case 0x0B: r_[reg] = value & 0x03; break;
        case 0x11: case 0x12: case 0x16: case 0x17: r_[reg] = value; break;
        case 0x13:
            r_[reg] = value & 0x7F;
            dataPortRead();
            break;
        case 0x14:
            r_[reg] = value;
            dataPortSeek(1);
            break;
        case 0x15:
            r_[reg] = value;
            if (r_[0x18] & 2) dataPortRead();
            dataPortSeek(2);
            break;
        case 0x18:
            r_[reg] = value & 0x7F;
            dataPortRead();
            break;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26: r_[reg] = value; break;
        case 0x25:
            r_[reg] = value;
            r_[0x2F] |= 0x81;
            aluMultiply();  // (30 clocks on the chip; the game waits on 482F)
            break;
        case 0x27:
            r_[reg] = value;
            r_[0x2F] |= 0x80;
            aluDivide();
            break;
        case 0x2E: r_[reg] = value & 0x01; break;
        case 0x30: r_[reg] = value & 0x87; break;
        case 0x31: case 0x32: case 0x33: case 0x34: r_[reg] = value & 0x07; break;
        default: break;
    }
}

// The decompression unit.

void Spc7110::dcuLoadAddress() {
    const uint32_t table = r_[0x01] | r_[0x02] << 8 | static_cast<uint32_t>(r_[0x03]) << 16;
    const uint32_t address = (table + (static_cast<uint32_t>(r_[0x04]) << 2)) & 0xFFFFFF;
    dcuMode_ = dataRead(address) & 3;
    dcuAddress_ = (static_cast<uint32_t>(dataRead(address + 1)) << 16 | dataRead(address + 2) << 8 |
                   dataRead(address + 3)) & 0x7FFFFF;
}

void Spc7110::dcuBeginTransfer() {
    if (dcuMode_ == 3) return;  // not a mode
    decompInit(dcuMode_, dcuAddress_);
    decompDecode();
    uint16_t seek = r_[0x0B] & 2 ? static_cast<uint16_t>(r_[0x05] | r_[0x06] << 8) : 0;
    while (seek--) decompDecode();
    r_[0x0C] |= 0x80;
    dcuOffset_ = 0;
}

uint8_t Spc7110::dcuRead() {
    if (!(r_[0x0C] & 0x80)) return 0;
    if (dcuOffset_ == 0) {
        for (int row = 0; row < 8; ++row) {
            const size_t r = static_cast<size_t>(row);
            switch (dBpp_) {
                case 1: dcuTile_[r] = static_cast<uint8_t>(dResult_); break;
                case 2:
                    dcuTile_[r * 2] = static_cast<uint8_t>(dResult_);
                    dcuTile_[r * 2 + 1] = static_cast<uint8_t>(dResult_ >> 8);
                    break;
                default:
                    dcuTile_[r * 2] = static_cast<uint8_t>(dResult_);
                    dcuTile_[r * 2 + 1] = static_cast<uint8_t>(dResult_ >> 8);
                    dcuTile_[r * 2 + 16] = static_cast<uint8_t>(dResult_ >> 16);
                    dcuTile_[r * 2 + 17] = static_cast<uint8_t>(dResult_ >> 24);
                    break;
            }
            uint8_t seek = r_[0x0B] & 1 ? r_[0x07] : 1;
            while (seek--) decompDecode();
        }
    }
    const uint8_t v = dcuTile_[dcuOffset_++];
    dcuOffset_ &= 8 * dBpp_ - 1;
    return v;
}

// The data port.

void Spc7110::setDataOffset(uint32_t v) {
    r_[0x11] = static_cast<uint8_t>(v);
    r_[0x12] = static_cast<uint8_t>(v >> 8);
    r_[0x13] = static_cast<uint8_t>((v >> 16) & 0x7F);
}

void Spc7110::setDataAdjust(uint32_t v) {
    r_[0x14] = static_cast<uint8_t>(v);
    r_[0x15] = static_cast<uint8_t>(v >> 8);
}

void Spc7110::dataPortRead() {
    uint32_t adjust = r_[0x18] & 2 ? dataAdjust() : 0;
    if (r_[0x18] & 8) adjust = static_cast<uint32_t>(static_cast<int16_t>(adjust));
    r_[0x10] = dataRead((dataOffset() + adjust) & 0xFFFFFF);
}

void Spc7110::dataPortIncrement() {
    uint32_t stride = r_[0x18] & 1 ? dataStride() : 1;
    uint32_t adjust = dataAdjust();
    if (r_[0x18] & 4) stride = static_cast<uint32_t>(static_cast<int16_t>(stride));
    if (r_[0x18] & 8) adjust = static_cast<uint32_t>(static_cast<int16_t>(adjust));
    if (r_[0x18] & 16) setDataAdjust(adjust + stride);
    else setDataOffset(dataOffset() + stride);
    dataPortRead();
}

void Spc7110::dataPortSeek(int mode) {
    if (r_[0x18] >> 5 != mode) return;
    uint32_t adjust = dataAdjust();
    if (r_[0x18] & 8) adjust = static_cast<uint32_t>(static_cast<int16_t>(adjust));
    setDataOffset(dataOffset() + adjust);
    dataPortRead();
}

// The arithmetic unit.

void Spc7110::aluMultiply() {
    const uint16_t a = static_cast<uint16_t>(r_[0x24] | r_[0x25] << 8), b = static_cast<uint16_t>(r_[0x20] | r_[0x21] << 8);
    const uint32_t result = r_[0x2E] & 1
                                ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int16_t>(b))
                                : static_cast<uint32_t>(a) * b;
    for (int i = 0; i < 4; ++i) r_[static_cast<size_t>(0x28 + i)] = static_cast<uint8_t>(result >> (8 * i));
    r_[0x2F] &= 0x7F;
}

void Spc7110::aluDivide() {
    const uint32_t dividend = r_[0x20] | r_[0x21] << 8 | r_[0x22] << 16 | static_cast<uint32_t>(r_[0x23]) << 24;
    const uint16_t divisor = static_cast<uint16_t>(r_[0x26] | r_[0x27] << 8);
    uint32_t quotient;
    uint16_t remainder;
    if (r_[0x2E] & 1) {
        const int32_t n = static_cast<int32_t>(dividend);
        const int16_t d = static_cast<int16_t>(divisor);
        if (d) {
            // (INT32_MIN / -1 would overflow: the chip's answer is the dividend)
            quotient = n == INT32_MIN && d == -1 ? dividend : static_cast<uint32_t>(n / d);
            remainder = n == INT32_MIN && d == -1 ? 0 : static_cast<uint16_t>(n % d);
        } else {
            quotient = 0;
            remainder = static_cast<uint16_t>(dividend);
        }
    } else if (divisor) {
        quotient = dividend / divisor;
        remainder = static_cast<uint16_t>(dividend % divisor);
    } else {
        quotient = 0;
        remainder = static_cast<uint16_t>(dividend);
    }
    for (int i = 0; i < 4; ++i) r_[static_cast<size_t>(0x28 + i)] = static_cast<uint8_t>(quotient >> (8 * i));
    r_[0x2C] = static_cast<uint8_t>(remainder);
    r_[0x2D] = static_cast<uint8_t>(remainder >> 8);
    r_[0x2F] &= 0x7F;
}

// The decompressor.

void Spc7110::decompInit(uint32_t mode, uint32_t origin) {
    ctxPrediction_.fill(0);
    ctxSwap_.fill(0);
    dBpp_ = 1u << mode;
    dOffset_ = origin;
    dBits_ = 8;
    dRange_ = kMax + 1;
    dInput_ = decompReadByte();
    dInput_ = static_cast<uint16_t>(dInput_ << 8 | decompReadByte());
    dOutput_ = 0;
    dPixels_ = 0;
    dColormap_ = 0xfedcba9876543210ull;
}

void Spc7110::decompDecode() {
    for (uint32_t pixel = 0; pixel < 8; ++pixel) {
        uint64_t map = dColormap_;
        uint32_t diff = 0;
        if (dBpp_ > 1) {
            const uint32_t pa = static_cast<uint32_t>(dBpp_ == 2 ? dPixels_ >> 2 & 3 : dPixels_ & 15);
            const uint32_t pb = static_cast<uint32_t>(dBpp_ == 2 ? dPixels_ >> 14 & 3 : dPixels_ >> 28 & 15);
            const uint32_t pc = static_cast<uint32_t>(dBpp_ == 2 ? dPixels_ >> 16 & 3 : dPixels_ >> 32 & 15);
            if (pa != pb || pb != pc) {
                const uint32_t match = pa ^ pb ^ pc;
                diff = 4;                        // all differ
                if ((match ^ pc) == 0) diff = 3;  // a == b
                if ((match ^ pb) == 0) diff = 2;  // c == a
                if ((match ^ pa) == 0) diff = 1;  // b == c
            }
            dColormap_ = moveToFront(dColormap_, pa);
            map = moveToFront(map, pc);
            map = moveToFront(map, pb);
            map = moveToFront(map, pa);
        }
        for (uint32_t plane = 0; plane < dBpp_; ++plane) {
            const uint32_t bit = dBpp_ > 1 ? 1u << plane : 1u << (pixel & 3);
            const uint32_t history = (bit - 1) & dOutput_;
            uint32_t set = 0;
            if (dBpp_ == 1) set = pixel >= 4;
            if (dBpp_ == 2) set = diff;
            if (plane >= 2 && history <= 1) set = diff;
            const size_t ctx = set * 15 + bit + history - 1;
            const Model& model = kEvolution[ctxPrediction_[ctx]];
            const uint8_t lpsOffset = static_cast<uint8_t>(dRange_ - model.probability);
            const uint32_t symbol = dInput_ >= (lpsOffset << 8) ? 1 : 0;
            dOutput_ = static_cast<uint8_t>(dOutput_ << 1 | (symbol ^ ctxSwap_[ctx]));
            if (symbol == kMps) {
                dRange_ = lpsOffset;
            } else {
                dRange_ = static_cast<uint16_t>(dRange_ - lpsOffset);
                dInput_ = static_cast<uint16_t>(dInput_ - (lpsOffset << 8));
            }
            while (dRange_ <= kMax / 2) {  // back into [0.75, 1.5]
                ctxPrediction_[ctx] = model.next[symbol];
                dRange_ = static_cast<uint16_t>(dRange_ << 1);
                dInput_ = static_cast<uint16_t>(dInput_ << 1);
                if (--dBits_ == 0) {
                    dBits_ = 8;
                    dInput_ = static_cast<uint16_t>(dInput_ + decompReadByte());
                }
            }
            if (symbol == kLps && model.probability > kHalf) ctxSwap_[ctx] ^= 1;
        }
        uint32_t index = dOutput_ & ((1u << dBpp_) - 1);
        if (dBpp_ == 1) index ^= static_cast<uint32_t>(dPixels_ >> 15 & 1);
        dPixels_ = dPixels_ << dBpp_ | (map >> (4 * index) & 15);
    }
    if (dBpp_ == 1) dResult_ = static_cast<uint32_t>(dPixels_);
    if (dBpp_ == 2) dResult_ = deinterleave(dPixels_, 16);
    if (dBpp_ == 4) dResult_ = deinterleave(deinterleave(dPixels_, 32), 32);
}

}  // namespace plumnin::snes
