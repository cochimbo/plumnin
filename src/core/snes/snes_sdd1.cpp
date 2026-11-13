#include "snes_sdd1.h"

namespace plumnin::snes {

namespace {

// The Golomb codes' run lengths, by code word.
constexpr uint8_t kRunCount[256] = {
    0x00, 0x00, 0x01, 0x00, 0x03, 0x01, 0x02, 0x00, 0x07, 0x03, 0x05, 0x01, 0x06, 0x02, 0x04, 0x00, 0x0f, 0x07, 0x0b,
    0x03, 0x0d, 0x05, 0x09, 0x01, 0x0e, 0x06, 0x0a, 0x02, 0x0c, 0x04, 0x08, 0x00, 0x1f, 0x0f, 0x17, 0x07, 0x1b, 0x0b,
    0x13, 0x03, 0x1d, 0x0d, 0x15, 0x05, 0x19, 0x09, 0x11, 0x01, 0x1e, 0x0e, 0x16, 0x06, 0x1a, 0x0a, 0x12, 0x02, 0x1c,
    0x0c, 0x14, 0x04, 0x18, 0x08, 0x10, 0x00, 0x3f, 0x1f, 0x2f, 0x0f, 0x37, 0x17, 0x27, 0x07, 0x3b, 0x1b, 0x2b, 0x0b,
    0x33, 0x13, 0x23, 0x03, 0x3d, 0x1d, 0x2d, 0x0d, 0x35, 0x15, 0x25, 0x05, 0x39, 0x19, 0x29, 0x09, 0x31, 0x11, 0x21,
    0x01, 0x3e, 0x1e, 0x2e, 0x0e, 0x36, 0x16, 0x26, 0x06, 0x3a, 0x1a, 0x2a, 0x0a, 0x32, 0x12, 0x22, 0x02, 0x3c, 0x1c,
    0x2c, 0x0c, 0x34, 0x14, 0x24, 0x04, 0x38, 0x18, 0x28, 0x08, 0x30, 0x10, 0x20, 0x00, 0x7f, 0x3f, 0x5f, 0x1f, 0x6f,
    0x2f, 0x4f, 0x0f, 0x77, 0x37, 0x57, 0x17, 0x67, 0x27, 0x47, 0x07, 0x7b, 0x3b, 0x5b, 0x1b, 0x6b, 0x2b, 0x4b, 0x0b,
    0x73, 0x33, 0x53, 0x13, 0x63, 0x23, 0x43, 0x03, 0x7d, 0x3d, 0x5d, 0x1d, 0x6d, 0x2d, 0x4d, 0x0d, 0x75, 0x35, 0x55,
    0x15, 0x65, 0x25, 0x45, 0x05, 0x79, 0x39, 0x59, 0x19, 0x69, 0x29, 0x49, 0x09, 0x71, 0x31, 0x51, 0x11, 0x61, 0x21,
    0x41, 0x01, 0x7e, 0x3e, 0x5e, 0x1e, 0x6e, 0x2e, 0x4e, 0x0e, 0x76, 0x36, 0x56, 0x16, 0x66, 0x26, 0x46, 0x06, 0x7a,
    0x3a, 0x5a, 0x1a, 0x6a, 0x2a, 0x4a, 0x0a, 0x72, 0x32, 0x52, 0x12, 0x62, 0x22, 0x42, 0x02, 0x7c, 0x3c, 0x5c, 0x1c,
    0x6c, 0x2c, 0x4c, 0x0c, 0x74, 0x34, 0x54, 0x14, 0x64, 0x24, 0x44, 0x04, 0x78, 0x38, 0x58, 0x18, 0x68, 0x28, 0x48,
    0x08, 0x70, 0x30, 0x50, 0x10, 0x60, 0x20, 0x40, 0x00,
};

// The probability states: the bit generator each uses, and the next state after a run
// of the likelier bit or after the other.
struct Evolution {
    uint8_t code, ifMps, ifLps;
};
constexpr Evolution kEvolution[33] = {
    {0, 25, 25}, {0, 2, 1},   {0, 3, 1},   {0, 4, 2},   {0, 5, 3},   {1, 6, 4},   {1, 7, 5},   {1, 8, 6},   {1, 9, 7},
    {2, 10, 8},  {2, 11, 9},  {2, 12, 10}, {2, 13, 11}, {3, 14, 12}, {3, 15, 13}, {3, 16, 14}, {3, 17, 15}, {4, 18, 16},
    {4, 19, 17}, {5, 20, 18}, {5, 21, 19}, {6, 22, 20}, {6, 23, 21}, {7, 24, 22}, {7, 24, 23}, {0, 26, 1},  {1, 27, 2},
    {2, 28, 4},  {3, 29, 8},  {4, 30, 12}, {5, 31, 16}, {6, 32, 18}, {7, 24, 22},
};

}  // namespace

void Sdd1::power() {
    r4800_ = r4801_ = 0;
    bank_ = {0, 1, 2, 3};
    dmaAddress_.fill(0);
    dmaSize_.fill(0);
    dmaReady_ = false;
}

uint8_t Sdd1::mmcRead(uint32_t address) const {
    return romAt(static_cast<uint32_t>(bank_[(address >> 20) & 3] & 0x0F) << 20 | (address & 0xFFFFF));
}

bool Sdd1::read(uint32_t address, uint8_t& value) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if (!(bank & 0x40)) {
        if ((bank & 0x7F) >= 0x40) return false;
        if (a >= 0x4800 && a < 0x4810) {
            switch (a & 0xF) {
                case 0: value = r4800_; return true;
                case 1: value = r4801_; return true;
                case 4: case 5: case 6: case 7: value = bank_[a & 3]; return true;
                default: return false;
            }
        }
        if (a >= 0x6000 && a < 0x8000 && !ram_.empty()) {
            value = ram_[(a & 0x1FFF) % ram_.size()];
            return true;
        }
        if (a < 0x8000) return false;
        // LoROM; with bit 7 of a bank register, 20-3F (or A0-BF) mirror 00-1F (80-9F).
        uint32_t b = bank & 0xBF;
        if ((b & 0x20) && (bank_[b & 0x80 ? 3 : 1] & 0x80)) b &= ~0x20u;
        value = romAt((b & 0x3F) << 15 | (a & 0x7FFF));
        return true;
    }
    if (bank < 0xC0) return false;
    if (r4800_ & r4801_) {
        for (int n = 0; n < 8; ++n) {
            const uint8_t bit = static_cast<uint8_t>(1 << n);
            if (!(r4800_ & r4801_ & bit) || address != dmaAddress_[static_cast<size_t>(n)]) continue;
            // A channel that decompresses always reads one fixed address.
            if (!dmaReady_) {
                decompressInit(address);
                dmaReady_ = true;
            }
            value = decompress();
            if (--dmaSize_[static_cast<size_t>(n)] == 0) {
                dmaReady_ = false;
                r4801_ &= static_cast<uint8_t>(~bit);
            }
            return true;
        }
    }
    value = mmcRead(address);
    return true;
}

bool Sdd1::write(uint32_t address, uint8_t value) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if ((bank & 0x40) || (bank & 0x7F) >= 0x40) return false;
    if (a >= 0x4300 && a < 0x4380) {
        // Watched, not taken: the console's DMA registers still get it.
        const size_t ch = (a >> 4) & 7;
        switch (a & 0xF) {
            case 2: dmaAddress_[ch] = (dmaAddress_[ch] & 0xFFFF00) | value; break;
            case 3: dmaAddress_[ch] = (dmaAddress_[ch] & 0xFF00FF) | static_cast<uint32_t>(value) << 8; break;
            case 4: dmaAddress_[ch] = (dmaAddress_[ch] & 0x00FFFF) | static_cast<uint32_t>(value) << 16; break;
            case 5: dmaSize_[ch] = static_cast<uint16_t>((dmaSize_[ch] & 0xFF00) | value); break;
            case 6: dmaSize_[ch] = static_cast<uint16_t>((dmaSize_[ch] & 0x00FF) | value << 8); break;
            default: break;
        }
        return false;
    }
    if (a >= 0x6000 && a < 0x8000 && !ram_.empty()) {
        uint8_t& b = ram_[(a & 0x1FFF) % ram_.size()];
        if (b != value) batteryChanged_ = true;
        b = value;
        return true;
    }
    if (a < 0x4800 || a >= 0x4810) return false;
    switch (a & 0xF) {
        case 0: r4800_ = value; break;
        case 1: r4801_ = value; break;
        case 4: case 5: case 6: case 7: bank_[a & 3] = value & 0x8F; break;
        default: break;
    }
    return true;
}

// The decompressor.

void Sdd1::decompressInit(uint32_t address) {
    imOffset_ = address;
    imBits_ = 4;
    bgMps_.fill(0);
    bgLps_.fill(false);
    ctxStatus_.fill(0);
    ctxMps_.fill(0);
    const uint8_t header = mmcRead(address);
    bitplanes_ = header & 0xC0;
    contextBits_ = header & 0x30;
    bitNumber_ = 0;
    previous_.fill(0);
    switch (bitplanes_) {
        case 0x00: bitplane_ = 1; break;
        case 0x40: bitplane_ = 7; break;
        case 0x80: bitplane_ = 3; break;
        default: bitplane_ = 0; break;
    }
    r0_ = 0x01;
    r1_ = r2_ = 0;
}

// The input: the next code word, from the bit where the last one ended.
uint8_t Sdd1::codeWord(uint8_t length) {
    uint8_t word = static_cast<uint8_t>(mmcRead(imOffset_) << imBits_);
    ++imBits_;
    if (word & 0x80) {
        word = static_cast<uint8_t>(word | mmcRead(imOffset_ + 1) >> (9 - imBits_));
        imBits_ = static_cast<uint8_t>(imBits_ + length);
    }
    if (imBits_ & 0x08) {
        ++imOffset_;
        imBits_ &= 0x07;
    }
    return word;
}

uint8_t Sdd1::bgBit(int code, bool& endOfRun) {
    uint8_t& mps = bgMps_[static_cast<size_t>(code)];
    bool& lps = bgLps_[static_cast<size_t>(code)];
    if (!mps && !lps) {
        const uint8_t word = codeWord(static_cast<uint8_t>(code));
        if (word & 0x80) {
            lps = true;
            mps = kRunCount[word >> (code ^ 7)];
        } else {
            mps = static_cast<uint8_t>(1 << code);
        }
    }
    uint8_t bit;
    if (mps) {
        bit = 0;
        --mps;
    } else {
        bit = 1;
        lps = false;
    }
    endOfRun = !mps && !lps;
    return bit;
}

uint8_t Sdd1::pemBit(uint8_t context) {
    uint8_t& status = ctxStatus_[context];
    uint8_t& mps = ctxMps_[context];
    const uint8_t current = status, currentMps = mps;
    const Evolution& s = kEvolution[current];
    bool endOfRun = false;
    const uint8_t bit = bgBit(s.code, endOfRun);
    if (endOfRun) {
        if (bit) {
            if (!(current & 0xFE)) mps ^= 1;
            status = s.ifLps;
        } else {
            status = s.ifMps;
        }
    }
    return bit ^ currentMps;
}

uint8_t Sdd1::cmBit() {
    switch (bitplanes_) {
        case 0x00: bitplane_ ^= 1; break;
        case 0x40:
            bitplane_ ^= 1;
            if (!(bitNumber_ & 0x7F)) bitplane_ = (bitplane_ + 2) & 7;
            break;
        case 0x80:
            bitplane_ ^= 1;
            if (!(bitNumber_ & 0x7F)) bitplane_ ^= 2;
            break;
        default: bitplane_ = bitNumber_ & 7; break;
    }
    uint16_t& bits = previous_[bitplane_];
    uint8_t context = static_cast<uint8_t>((bitplane_ & 1) << 4);
    switch (contextBits_) {
        case 0x00: context |= ((bits & 0x01C0) >> 5) | (bits & 0x0001); break;
        case 0x10: context |= ((bits & 0x0180) >> 5) | (bits & 0x0001); break;
        case 0x20: context |= ((bits & 0x00C0) >> 5) | (bits & 0x0001); break;
        default: context |= ((bits & 0x0180) >> 5) | (bits & 0x0003); break;
    }
    const uint8_t bit = pemBit(context);
    bits = static_cast<uint16_t>(bits << 1 | bit);
    ++bitNumber_;
    return bit;
}

// The output: two bitplanes at a time (2, 4 and 8 bpp tiles), or the eight of a mode 7
// pixel.
uint8_t Sdd1::decompress() {
    if (bitplanes_ != 0xC0) {
        if (r0_ == 0) {
            r0_ = 0xFF;
            return r2_;
        }
        r1_ = r2_ = 0;
        for (r0_ = 0x80; r0_; r0_ >>= 1) {
            if (cmBit()) r1_ |= r0_;
            if (cmBit()) r2_ |= r0_;
        }
        return r1_;
    }
    r1_ = 0;
    for (r0_ = 0x01; r0_; r0_ = static_cast<uint8_t>(r0_ << 1))
        if (cmBit()) r1_ |= r0_;
    return r1_;
}

}  // namespace plumnin::snes
