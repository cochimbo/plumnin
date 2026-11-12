#include "nes_ppu.h"

#include <algorithm>

namespace plumnin::nes {

namespace {

// The 2C02's colours, as a TV shows them.
constexpr uint32_t kColours[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00, 0x333500, 0x0B4800, 0x005200,
    0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000, 0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B,
    0xB53120, 0x994E00, 0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000, 0xFFFEFF,
    0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22, 0xBCBE00, 0x88D800, 0x5CE430, 0x45E082,
    0x48CDDE, 0x4F4F4F, 0x000000, 0x000000, 0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5,
    0xF7D8A5, 0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000};

// The palette RAM as a NES powers on (what blargg's power_up_palette expects).
constexpr uint8_t kPowerPalette[32] = {0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08,
                                       0x24, 0x00, 0x00, 0x04, 0x2C, 0x09, 0x01, 0x34, 0x03, 0x00, 0x04,
                                       0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};

constexpr uint32_t kOpenBusDecayFrames = 30;

}  // namespace

Ppu::Ppu() { power(false); }

void Ppu::power(bool pal) {
    pal_ = pal;
    lastLine_ = pal ? 311 : 261;
    // Emphasis darkens the two colours it does not bring out (red and green swap on PAL).
    for (int e = 0; e < 8; ++e)
        for (int c = 0; c < 64; ++c) {
            const uint32_t base = kColours[c];
            double r = (base >> 16) & 0xFF, g = (base >> 8) & 0xFF, b = base & 0xFF;
            const int red = pal ? 2 : 1, green = pal ? 1 : 2;
            if ((c & 0x0E) != 0x0E && e) {
                if (!(e & red)) r *= 0.816;
                if (!(e & green)) g *= 0.816;
                if (!(e & 4)) b *= 0.816;
            }
            rgb_[static_cast<size_t>(e << 6 | c)] = 0xFF000000u | static_cast<uint32_t>(r) << 16 |
                                                     static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b);
        }
    vram.fill(0);
    oam.fill(0);
    secondary_.fill(0xFF);
    std::copy(std::begin(kPowerPalette), std::end(kPowerPalette), palette.begin());
    ctrl_ = mask_ = status_ = oamAddr_ = 0;
    v_ = t_ = 0;
    x_ = 0;
    w_ = false;
    buffer_ = openBus_ = 0;
    openBusFrame_.fill(0);
    scanline_ = static_cast<int16_t>(lastLine_);
    dot_ = 340;
    odd_ = true;
    frames_ = 0;
    dots_ = 0;
    frameDone_ = preventVbl_ = skipDot_ = false;
    ignoreRead_ = 0;
    pendingDelay_ = 0;
    pendingV_ = 0;
    ntByte_ = atByte_ = loByte_ = hiByte_ = 0;
    bgLo_ = bgHi_ = atLo_ = atHi_ = 0;
    copyBuffer_ = secAddr_ = spriteH_ = spriteL_ = 0;
    inRange_ = copyDone_ = sprite0Added_ = sprite0Line_ = false;
    overflowCount_ = foundSprites_ = lineSprites_ = 0;
}

void Ppu::reset() {
    ctrl_ = mask_ = 0;
    w_ = false;
    buffer_ = 0;
    t_ = 0;
    x_ = 0;
}

uint8_t Ppu::busRead(uint16_t a) const {
    a &= 0x3FFF;
    if (a < 0x2000) return mapper_->chrRead(a);
    return vram[static_cast<size_t>(mapper_->nametable(a)) * 0x400 + (a & 0x3FF)];
}

void Ppu::busWrite(uint16_t a, uint8_t value) {
    a &= 0x3FFF;
    if (a < 0x2000) mapper_->chrWrite(a, value);
    else vram[static_cast<size_t>(mapper_->nametable(a)) * 0x400 + (a & 0x3FF)] = value;
}

void Ppu::setBusAddress(uint16_t a) {
    if (mapper_->watchesPpu()) mapper_->ppuAddress(a & 0x3FFF, dots_);
}

uint8_t Ppu::fetch(uint16_t a) {
    const uint8_t v = busRead(a);
    if (mapper_->watchesPpu()) mapper_->ppuAddress(a, dots_);
    return v;
}

uint8_t Ppu::paletteRead(uint16_t a) const {
    size_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;
    return palette[i];
}

uint8_t Ppu::decayedOpenBus(uint8_t mask) {
    for (int b = 0; b < 8; ++b)
        if ((mask >> b) & 1 && frames_ - openBusFrame_[static_cast<size_t>(b)] > kOpenBusDecayFrames)
            openBus_ = static_cast<uint8_t>(openBus_ & ~(1 << b));
    return openBus_ & mask;
}

void Ppu::driveOpenBus(uint8_t value, uint8_t mask) {
    openBus_ = static_cast<uint8_t>((openBus_ & ~mask) | (value & mask));
    for (int b = 0; b < 8; ++b)
        if ((mask >> b) & 1) openBusFrame_[static_cast<size_t>(b)] = frames_;
}

void Ppu::incX() {
    if ((v_ & 0x1F) == 31) v_ = static_cast<uint16_t>((v_ & ~0x1F) ^ 0x400);
    else ++v_;
}

void Ppu::incY() {
    if ((v_ & 0x7000) != 0x7000) {
        v_ = static_cast<uint16_t>(v_ + 0x1000);
        return;
    }
    v_ &= 0x0FFF;
    int y = (v_ >> 5) & 31;
    if (y == 29) y = 0, v_ ^= 0x800;
    else if (y == 31) y = 0;
    else ++y;
    v_ = static_cast<uint16_t>((v_ & ~0x3E0) | y << 5);
}

void Ppu::incrementV() {
    if (rendering() && (scanline_ < 240 || scanline_ == lastLine_)) {
        // During rendering 2007 bumps both scroll counters at once.
        incX();
        incY();
    } else {
        v_ = static_cast<uint16_t>((v_ + (ctrl_ & 4 ? 32 : 1)) & 0x7FFF);
        setBusAddress(v_);
    }
}

uint8_t Ppu::peek(uint16_t reg) const {
    switch (reg & 7) {
        case 2: return static_cast<uint8_t>((status_ & 0xE0) | (openBus_ & 0x1F));
        case 4: return oam[oamAddr_];
        case 7: return (v_ & 0x3FFF) >= 0x3F00 ? paletteRead(v_) : buffer_;
        default: return openBus_;
    }
}

uint8_t Ppu::read(uint16_t reg) {
    switch (reg & 7) {
        case 2: {
            const uint8_t v = static_cast<uint8_t>((status_ & 0xE0) | decayedOpenBus(0x1F));
            status_ &= 0x7F;
            w_ = false;
            // Read a dot before vblank starts: the flag reads clear and never sets this frame.
            if (scanline_ == 241 && dot_ == 0) preventVbl_ = true;
            driveOpenBus(v, 0xE0);
            return v;
        }
        case 4: {
            uint8_t v;
            if (rendering() && scanline_ < 240) {
                if (dot_ >= 257 && dot_ <= 320) {
                    const int step = std::min(3, (dot_ - 257) & 7);
                    secAddr_ = static_cast<uint8_t>((dot_ - 257) / 8 * 4 + step);
                    copyBuffer_ = secondary_[secAddr_];
                }
                v = copyBuffer_;
            } else {
                v = oam[oamAddr_];
            }
            driveOpenBus(v, 0xFF);
            return v;
        }
        case 7: {
            // A second read right after one (a dummy read on the same address) is not
            // seen: the bus keeps the last value.
            if (ignoreRead_) return openBus_;
            ignoreRead_ = 6;
            const uint16_t a = v_ & 0x3FFF;
            uint8_t v;
            if (a >= 0x3F00) {
                v = static_cast<uint8_t>((paletteRead(a) & (mask_ & 1 ? 0x30 : 0x3F)) | decayedOpenBus(0xC0));
                buffer_ = busRead(static_cast<uint16_t>(a - 0x1000));
                driveOpenBus(v, 0x3F);
            } else {
                v = buffer_;
                setBusAddress(a);
                buffer_ = busRead(a);
                driveOpenBus(v, 0xFF);
            }
            incrementV();
            return v;
        }
        default: return decayedOpenBus(0xFF);
    }
}

void Ppu::write(uint16_t reg, uint8_t value) {
    driveOpenBus(value, 0xFF);
    switch (reg & 7) {
        case 0:
            ctrl_ = value;
            t_ = static_cast<uint16_t>((t_ & 0x73FF) | (value & 3) << 10);
            break;
        case 1: mask_ = value; break;
        case 3: oamAddr_ = value; break;
        case 4:
            if (rendering() && (scanline_ < 240 || scanline_ == lastLine_)) {
                oamAddr_ = static_cast<uint8_t>(oamAddr_ + 4);  // a glitchy increment, nothing written
            } else {
                if ((oamAddr_ & 3) == 2) value &= 0xE3;
                oam[oamAddr_++] = value;
            }
            break;
        case 5:
            if (!w_) {
                t_ = static_cast<uint16_t>((t_ & 0x7FE0) | value >> 3);
                x_ = value & 7;
            } else {
                t_ = static_cast<uint16_t>((t_ & 0x0C1F) | (value & 7) << 12 | (value & 0xF8) << 2);
            }
            w_ = !w_;
            break;
        case 6:
            if (!w_) {
                t_ = static_cast<uint16_t>((t_ & 0x00FF) | (value & 0x3F) << 8);
            } else {
                t_ = static_cast<uint16_t>((t_ & 0x7F00) | value);
                pendingV_ = t_;
                pendingDelay_ = 3;
            }
            w_ = !w_;
            break;
        default: {
            const uint16_t a = v_ & 0x3FFF;
            if (a >= 0x3F00) {
                size_t i = a & 0x1F;
                if ((i & 0x13) == 0x10) i &= 0x0F;
                palette[i] = value & 0x3F;
            } else {
                setBusAddress(a);
                busWrite(a, value);
            }
            incrementV();
            break;
        }
    }
}

void Ppu::backgroundDot() {
    bgLo_ = static_cast<uint16_t>(bgLo_ << 1);
    bgHi_ = static_cast<uint16_t>(bgHi_ << 1);
    atLo_ = static_cast<uint16_t>(atLo_ << 1);
    atHi_ = static_cast<uint16_t>(atHi_ << 1);
    switch ((dot_ - 1) & 7) {
        case 0: ntByte_ = fetch(static_cast<uint16_t>(0x2000 | (v_ & 0x0FFF))); break;
        case 2: {
            const uint8_t a =
                fetch(static_cast<uint16_t>(0x23C0 | (v_ & 0x0C00) | ((v_ >> 4) & 0x38) | ((v_ >> 2) & 7)));
            atByte_ = (a >> (((v_ >> 4) & 4) | (v_ & 2))) & 3;
            break;
        }
        case 4:
            loByte_ = fetch(static_cast<uint16_t>((ctrl_ & 0x10 ? 0x1000 : 0) | ntByte_ << 4 | ((v_ >> 12) & 7)));
            break;
        case 6:
            hiByte_ = fetch(static_cast<uint16_t>((ctrl_ & 0x10 ? 0x1000 : 0) | ntByte_ << 4 | ((v_ >> 12) & 7) | 8));
            break;
        case 7:
            bgLo_ |= loByte_;
            bgHi_ |= hiByte_;
            atLo_ |= atByte_ & 1 ? 0xFF : 0;
            atHi_ |= atByte_ & 2 ? 0xFF : 0;
            incX();
            break;
        default: break;
    }
}

// The next line's sprites, a byte of OAM at a time: secondary OAM cleared on dots
// 1-64, then filled with up to eight sprites, and the overflow flag set by the
// hardware's buggy search past the eighth.
void Ppu::evaluateDot() {
    if (dot_ <= 64) {
        copyBuffer_ = 0xFF;
        secondary_[static_cast<size_t>((dot_ - 1) >> 1)] = 0xFF;
        return;
    }
    if (dot_ == 65) {
        sprite0Added_ = inRange_ = copyDone_ = false;
        secAddr_ = 0;
        overflowCount_ = 0;
        spriteH_ = (oamAddr_ >> 2) & 0x3F;
        spriteL_ = oamAddr_ & 3;
    } else if (dot_ == 256) {
        foundSprites_ = secAddr_ >> 2;
    }
    if (dot_ & 1) {
        copyBuffer_ = oam[oamAddr_];
        return;
    }
    const int height = ctrl_ & 0x20 ? 16 : 8;
    if (copyDone_) {
        spriteH_ = (spriteH_ + 1) & 0x3F;
        if (secAddr_ >= 0x20) copyBuffer_ = secondary_[secAddr_ & 0x1F];
    } else {
        if (!inRange_ && scanline_ >= copyBuffer_ && scanline_ < copyBuffer_ + height) inRange_ = true;
        if (secAddr_ < 0x20) {
            secondary_[secAddr_] = copyBuffer_;
            if (inRange_) {
                ++spriteL_;
                ++secAddr_;
                if (spriteH_ == 0) sprite0Added_ = true;
                if ((secAddr_ & 3) == 0) {
                    secIndex_[static_cast<size_t>((secAddr_ >> 2) - 1)] = spriteH_;
                    inRange_ = false;
                    spriteL_ = 0;
                    spriteH_ = (spriteH_ + 1) & 0x3F;
                    if (spriteH_ == 0) copyDone_ = true;
                }
            } else {
                spriteH_ = (spriteH_ + 1) & 0x3F;
                if (spriteH_ == 0) copyDone_ = true;
            }
        } else {
            copyBuffer_ = secondary_[secAddr_ & 0x1F];
            if (inRange_) {
                status_ |= 0x20;
                if (++spriteL_ == 4) {
                    spriteH_ = (spriteH_ + 1) & 0x3F;
                    spriteL_ = 0;
                }
                if (overflowCount_ == 0) {
                    overflowCount_ = 3;
                } else if (--overflowCount_ == 0) {
                    copyDone_ = true;
                    spriteL_ = 0;
                }
            } else {
                // Not on this line: the bug moves to the next sprite and its next byte.
                spriteH_ = (spriteH_ + 1) & 0x3F;
                spriteL_ = (spriteL_ + 1) & 3;
                if (spriteH_ == 0) copyDone_ = true;
            }
        }
    }
    oamAddr_ = static_cast<uint8_t>((spriteL_ & 3) | spriteH_ << 2);
}

void Ppu::spriteFetch() {
    const int slot = (dot_ - 257) >> 3, phase = (dot_ - 257) & 7;
    const bool real = slot < foundSprites_ && scanline_ < 240;
    const uint8_t* s = &secondary_[static_cast<size_t>(slot * 4)];
    auto address = [&]() -> uint16_t {
        const bool big = ctrl_ & 0x20;
        int row = 0, tile = 0xFF;
        if (real) {
            row = (scanline_ - s[0]) & 0xFF;
            tile = s[1];
            if (s[2] & 0x80) row = (big ? 15 : 7) - row;
        }
        if (big) return static_cast<uint16_t>((tile & 1 ? 0x1000 : 0) | (tile & 0xFE) << 4 | (row & 7) | (row & 8) << 1);
        return static_cast<uint16_t>((ctrl_ & 0x08 ? 0x1000 : 0) | tile << 4 | (row & 7));
    };
    switch (phase) {
        case 0:
        case 2: fetch(static_cast<uint16_t>(0x2000 | (v_ & 0x0FFF))); break;
        case 4: loByte_ = fetch(address()); break;
        case 6: {
            hiByte_ = fetch(static_cast<uint16_t>(address() + 8));
            if (!real) break;
            uint8_t lo = loByte_, hi = hiByte_;
            if (s[2] & 0x40) {  // flipped: the bits the other way round
                auto flip = [](uint8_t b) {
                    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
                    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
                    return static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
                };
                lo = flip(lo), hi = flip(hi);
            }
            const size_t i = static_cast<size_t>(slot);
            spX_[i] = s[3];
            spAttr_[i] = s[2];
            spLo_[i] = lo;
            spHi_[i] = hi;
            spIndex_[i] = secIndex_[i];
            break;
        }
        default: break;
    }
}

// With unlimited sprites, those past the eighth of the next line are drawn too (not
// fetched on the bus, and not counted for the sprite 0 hit).
void Ppu::extraSprites() {
    const int height = ctrl_ & 0x20 ? 16 : 8;
    int seen = 0;
    size_t n = lineSprites_;
    for (int i = 0; i < 64 && n < spX_.size(); ++i) {
        const uint8_t* s = &oam[static_cast<size_t>(i * 4)];
        const int row0 = scanline_ - s[0];
        if (row0 < 0 || row0 >= height) continue;
        if (++seen <= 8) continue;
        int row = s[2] & 0x80 ? height - 1 - row0 : row0;
        uint16_t a;
        if (height == 16) a = static_cast<uint16_t>((s[1] & 1 ? 0x1000 : 0) | (s[1] & 0xFE) << 4 | (row & 7) | (row & 8) << 1);
        else a = static_cast<uint16_t>((ctrl_ & 0x08 ? 0x1000 : 0) | s[1] << 4 | row);
        uint8_t lo = busRead(a), hi = busRead(static_cast<uint16_t>(a + 8));
        if (s[2] & 0x40) {
            uint8_t rl = 0, rh = 0;
            for (int b = 0; b < 8; ++b) rl = static_cast<uint8_t>(rl | ((lo >> b) & 1) << (7 - b)), rh = static_cast<uint8_t>(rh | ((hi >> b) & 1) << (7 - b));
            lo = rl, hi = rh;
        }
        spX_[n] = s[3];
        spAttr_[n] = s[2];
        spLo_[n] = lo;
        spHi_[n] = hi;
        spIndex_[n] = static_cast<uint8_t>(i);
        ++n;
    }
    lineSprites_ = static_cast<uint8_t>(n);
}

void Ppu::pixel() {
    const int x = dot_ - 1;
    int bgIdx = 0, bgPal = 0;
    if ((mask_ & 0x08) && (x >= 8 || (mask_ & 0x02))) {
        const int bit = 15 - x_;
        bgIdx = ((bgLo_ >> bit) & 1) | ((bgHi_ >> bit) & 1) << 1;
        bgPal = ((atLo_ >> bit) & 1) | ((atHi_ >> bit) & 1) << 1;
    }
    int spIdx = 0, spPal = 0, slot = -1;
    if ((mask_ & 0x10) && (x >= 8 || (mask_ & 0x04))) {
        for (int i = 0; i < lineSprites_; ++i) {
            const int off = x - spX_[static_cast<size_t>(i)];
            if (off < 0 || off > 7) continue;
            const int bit = 7 - off;
            const int idx = ((spLo_[static_cast<size_t>(i)] >> bit) & 1) | ((spHi_[static_cast<size_t>(i)] >> bit) & 1) << 1;
            if (!idx) continue;
            if (i == 0 && sprite0Line_ && bgIdx && x != 255) status_ |= 0x40;
            slot = i;
            spIdx = idx;
            spPal = spAttr_[static_cast<size_t>(i)] & 3;
            break;
        }
    }
    output(x, bgIdx, bgPal, spIdx, spPal, slot);
}

void Ppu::output(int x, int bgIdx, int bgPal, int spIdx, int spPal, int slot) {
    if (!drawing_ || !frame_) return;
    const int row = scanline_ - kFirstShownLine;
    if (row < 0 || row >= kShownLines) return;
    VideoFrame& f = *frame_;
    const size_t i = static_cast<size_t>(row) * VideoFrame::kMaxWidth + static_cast<size_t>(x);
    const size_t emph = static_cast<size_t>(mask_ & 0xE0) << 1;
    const uint8_t gray = mask_ & 1 ? 0x30 : 0x3F;
    auto colour = [&](uint8_t c) { return rgb_[emph | (c & gray)]; };
    const uint32_t backdrop = colour(palette[0]);
    if (x == 0) {
        f.backdrop[static_cast<size_t>(row)] = backdrop;
        const int16_t scroll = static_cast<int16_t>(-(((v_ & 0x1F) << 3 | x_) + ((v_ >> 10) & 1) * 256 - 16));
        f.lines[static_cast<size_t>(row)] = {scroll, scroll, rendering()};
    }
    f.planeA[i] = 0;
    f.planeAInfo[i] = Pixel{};
    uint32_t top = backdrop;
    Pixel topInfo = Pixel{};
    if (bgIdx) {
        const uint32_t c = colour(palette[static_cast<size_t>(bgPal * 4 + bgIdx)]);
        f.planeB[i] = c;
        f.planeBInfo[i] = Pixel{static_cast<uint8_t>(bgPal * 16 + bgIdx), Layer::Back, false, Shade::Normal, ntByte_};
        top = c;
        topInfo = f.planeBInfo[i];
    } else {
        f.planeB[i] = 0;
        f.planeBInfo[i] = Pixel{};
    }
    if (spIdx) {
        const size_t s = static_cast<size_t>(slot);
        const bool behind = spAttr_[s] & 0x20;
        const uint32_t c = colour(palette[static_cast<size_t>(16 + spPal * 4 + spIdx)]);
        f.sprites[i] = c;
        f.spriteInfo[i] =
            Pixel{static_cast<uint8_t>((4 + spPal) * 16 + spIdx), Layer::Sprite, !behind, Shade::Normal, spIndex_[s]};
        if (!bgIdx || !behind) {
            top = c;
            topInfo = f.spriteInfo[i];
        }
    } else {
        f.sprites[i] = 0;
        f.spriteInfo[i] = Pixel{};
    }
    f.rgba[i] = top;
    f.info[i] = topInfo;
}

// Rendering off: the backdrop, or the palette entry v points at.
void Ppu::blankPixel() {
    if (!drawing_ || !frame_) return;
    const int row = scanline_ - kFirstShownLine, x = dot_ - 1;
    if (row < 0 || row >= kShownLines) return;
    VideoFrame& f = *frame_;
    const size_t i = static_cast<size_t>(row) * VideoFrame::kMaxWidth + static_cast<size_t>(x);
    const uint8_t c = (v_ & 0x3F00) == 0x3F00 ? paletteRead(v_) : palette[0];
    const uint32_t rgb = rgb_[static_cast<size_t>(mask_ & 0xE0) << 1 | (c & (mask_ & 1 ? 0x30 : 0x3F))];
    if (x == 0) {
        f.backdrop[static_cast<size_t>(row)] = rgb;
        f.lines[static_cast<size_t>(row)] = {0, 0, false};
    }
    f.planeA[i] = f.planeB[i] = f.sprites[i] = 0;
    f.planeAInfo[i] = f.planeBInfo[i] = f.spriteInfo[i] = f.info[i] = Pixel{};
    f.rgba[i] = rgb;
}

void Ppu::step() {
    ++dots_;
    if (ignoreRead_) --ignoreRead_;
    if (++dot_ > 340) {
        dot_ = 0;
        if (++scanline_ > lastLine_) {
            scanline_ = 0;
            odd_ = !odd_;
            ++frames_;
        }
        if (scanline_ == 241) frameDone_ = true;
    }

    // NTSC: odd frames are a dot shorter when rendering was on at dot 338 of the
    // pre-render line (line 0's idle dot 0 is skipped).
    if (scanline_ == lastLine_ && dot_ == 338) skipDot_ = odd_ && !pal_ && rendering();
    if (scanline_ == lastLine_ && dot_ == 340 && skipDot_) {
        skipDot_ = false;
        dot_ = 0;
        scanline_ = 0;
        odd_ = !odd_;
        ++frames_;
    }
    const bool visible = scanline_ < 240, pre = scanline_ == lastLine_;
    if (pendingDelay_ && --pendingDelay_ == 0) {
        if ((visible || pre) && rendering()) {
            // Landing on a scroll increment, the write is ANDed with its result.
            if (dot_ == 257) v_ &= pendingV_;
            else if (dot_ > 0 && (dot_ & 7) == 0 && (dot_ <= 256 || dot_ > 320))
                v_ = static_cast<uint16_t>((pendingV_ & ~0x41F) | (v_ & pendingV_ & 0x41F));
            else v_ = pendingV_;
        } else {
            v_ = pendingV_;
            setBusAddress(v_);
        }
    }

    if (visible || pre) {
        if (pre && dot_ == 1) status_ &= 0x1F;  // vblank, sprite 0 hit and overflow end
        if (!rendering()) {
            if (visible && dot_ >= 1 && dot_ <= 256) blankPixel();
            return;
        }
        if (dot_ >= 1 && dot_ <= 256) {
            if (visible) pixel();
            backgroundDot();
            if (visible) evaluateDot();
            if (dot_ == 256) incY();
        } else if (dot_ >= 257 && dot_ <= 320) {
            if (dot_ == 257) {
                v_ = static_cast<uint16_t>((v_ & ~0x41F) | (t_ & 0x41F));
                lineSprites_ = visible ? foundSprites_ : 0;
                sprite0Line_ = visible && sprite0Added_;
            }
            if (pre && dot_ >= 280 && dot_ <= 304) v_ = static_cast<uint16_t>((v_ & ~0x7BE0) | (t_ & 0x7BE0));
            oamAddr_ = 0;
            spriteFetch();
            if (dot_ == 320 && visible && unlimited_) extraSprites();
        } else if (dot_ >= 321 && dot_ <= 336) {
            backgroundDot();
        } else if (dot_ == 337 || dot_ == 339) {
            ntByte_ = fetch(static_cast<uint16_t>(0x2000 | (v_ & 0x0FFF)));
        }
        return;
    }
    if (scanline_ == 241 && dot_ == 1) {
        if (!preventVbl_) status_ |= 0x80;
        preventVbl_ = false;
    }
}

}  // namespace plumnin::nes
