#include "gb_ppu.h"

#include <algorithm>
#include <cstring>

namespace plumnin::gb {

void Ppu::reset(bool color) {
    color_ = color;
    std::memset(vram, 0, sizeof vram);
    std::memset(oam, 0, sizeof oam);
    vramBank = 0;
    lcdc_ = 0x91;
    stat_ = 0;
    scy_ = scx_ = 0;
    lyc_ = 0;
    bgp_ = 0xFC;
    obp_[0] = obp_[1] = 0xFF;
    wy_ = wx_ = 0;
    // Where the boot ROM leaves it: the first frame's vertical blank about to end.
    line_ = 0;
    ly_ = 0;
    dot_ = 0;
    mode_ = 2;
    mode3End_ = 252;
    statLine_ = false;
    wyHit_ = false;
    winLine_ = 0;
    firstLine_ = false;
    offDots_ = 0;
    frameDone_ = hblank_ = false;
    bcps_ = ocps_ = 0;
    // The Color's palettes start white.
    std::memset(bgPal_, 0xFF, sizeof bgPal_);
    std::memset(obPal_, 0xFF, sizeof obPal_);
    opri_ = color ? 0 : 1;
    unwrappedScx_ = 0;
    spriteCount_ = 0;
}

uint32_t Ppu::cgbColour(const uint8_t* pal, int palette, int index) const {
    const int c = pal[palette * 8 + index * 2] | pal[palette * 8 + index * 2 + 1] << 8;
    auto ch = [](int v) { return static_cast<uint32_t>(v << 3 | v >> 2); };
    return 0xFF000000u | ch(c & 31) << 16 | ch((c >> 5) & 31) << 8 | ch((c >> 10) & 31);
}

void Ppu::updateStat(uint8_t& irq) {
    const bool line = ((stat_ & 0x40) && ly_ == lyc_) || ((stat_ & 0x08) && mode_ == 0) ||
                      ((stat_ & 0x10) && mode_ == 1) || ((stat_ & 0x20) && mode_ == 2);
    if (line && !statLine_) irq |= Stat;
    statLine_ = line;
}

void Ppu::setMode(int mode, uint8_t& irq) {
    mode_ = mode;
    updateStat(irq);
}

int Ppu::mode3Length() {
    // The sprites on this line: the first ten in OAM that cover it.
    const int height = lcdc_ & 4 ? 16 : 8;
    spriteCount_ = 0;
    for (uint8_t n = 0; n < 40 && spriteCount_ < 10; ++n) {
        const int y = oam[n * 4] - 16;
        if (line_ >= y && line_ < y + height) sprites_[spriteCount_++] = n;
    }
    // The Game Boy draws the one more to the left first (the first in OAM on a tie), the
    // Color the first in OAM.
    if (opri_ & 1)
        std::stable_sort(sprites_, sprites_ + spriteCount_, [&](uint8_t a, uint8_t b) { return oam[a * 4 + 1] < oam[b * 4 + 1]; });

    int length = 172 + (scx_ & 7);
    if ((lcdc_ & 0x20) && wyHit_ && wx_ < 167) length += 6;
    if (lcdc_ & 2)
        for (int i = 0; i < spriteCount_; ++i) {
            const int x = oam[sprites_[i] * 4 + 1];
            if (x >= 168) continue;
            const int t = (x + scx_) & 7;
            length += 6 + std::max(0, 5 - t);
        }
    return std::min(length, 289);
}

void Ppu::run(int dots, uint8_t& irq) {
    for (int n = 0; n < dots; ++n) {
        if (!(lcdc_ & 0x80)) {
            if (++offDots_ >= kDotsPerFrame) {
                offDots_ = 0;
                blankFrame();
                frameDone_ = true;
            }
            continue;
        }
        ++dot_;
        if (line_ < kScreenHeight) {
            if (dot_ == 80) {
                mode3End_ = 80 + mode3Length();
                setMode(3, irq);
            } else if (dot_ == mode3End_) {
                renderLine();
                setMode(0, irq);
                hblank_ = true;
            }
        } else if (line_ == 153 && dot_ == 4) {
            ly_ = 0;  // the last line reads as line 0 almost from its start
            updateStat(irq);
        }
        if (dot_ < kDotsPerLine) continue;
        dot_ = 0;
        firstLine_ = false;
        line_ = (line_ + 1) % kLines;
        ly_ = static_cast<uint8_t>(line_);
        if (line_ < kScreenHeight) {
            if (line_ == 0) {
                wyHit_ = false;
                winLine_ = 0;
            }
            if (ly_ == wy_) wyHit_ = true;
            setMode(2, irq);
        } else if (line_ == kScreenHeight) {
            irq |= VBlank;
            frameDone_ = true;
            // The OAM interrupt also fires as the vertical blank starts.
            if ((stat_ & 0x20) && !statLine_) irq |= Stat;
            setMode(1, irq);
        } else {
            updateStat(irq);
        }
    }
}

uint8_t Ppu::read(uint16_t address) const {
    switch (address) {
        case 0xFF40: return lcdc_;
        case 0xFF41: {
            const int mode = (lcdc_ & 0x80) ? (firstLine_ && mode_ == 2 ? 0 : mode_) : 0;
            return static_cast<uint8_t>(0x80 | (stat_ & 0x78) | (ly_ == lyc_ && (lcdc_ & 0x80) ? 4 : 0) | mode);
        }
        case 0xFF42: return scy_;
        case 0xFF43: return scx_;
        case 0xFF44: return ly_;
        case 0xFF45: return lyc_;
        case 0xFF47: return bgp_;
        case 0xFF48: return obp_[0];
        case 0xFF49: return obp_[1];
        case 0xFF4A: return wy_;
        case 0xFF4B: return wx_;
        default: break;
    }
    if (!color_) return 0xFF;
    switch (address) {
        case 0xFF4F: return 0xFE | vramBank;
        case 0xFF68: return bcps_ | 0x40;
        case 0xFF69: return bgPal_[bcps_ & 63];
        case 0xFF6A: return ocps_ | 0x40;
        case 0xFF6B: return obPal_[ocps_ & 63];
        case 0xFF6C: return 0xFE | opri_;
        default: return 0xFF;
    }
}

void Ppu::write(uint16_t address, uint8_t value, uint8_t& irq) {
    switch (address) {
        case 0xFF40: {
            const bool was = lcdc_ & 0x80;
            lcdc_ = value;
            if (was && !(value & 0x80)) {
                line_ = dot_ = 0;
                ly_ = 0;
                mode_ = 0;
                offDots_ = 0;
                statLine_ = false;
            } else if (!was && (value & 0x80)) {
                line_ = dot_ = 0;
                ly_ = 0;
                mode_ = 2;
                firstLine_ = true;
                wyHit_ = wy_ == 0;
                winLine_ = 0;
                // The first line has no OAM search: what reads as mode 2 is mode 0.
                const int saved = stat_;
                if (stat_ & 0x20) stat_ &= ~0x20;
                updateStat(irq);
                stat_ = static_cast<uint8_t>(saved);
            }
            return;
        }
        case 0xFF41:
            // The Game Boy's STAT bug: the write sets every source for a moment, which
            // fires the interrupt in the horizontal and vertical blanks.
            if (!color_ && (lcdc_ & 0x80) && (mode_ == 0 || mode_ == 1) && !statLine_) irq |= Stat;
            stat_ = value & 0x78;
            if (lcdc_ & 0x80) updateStat(irq);
            return;
        case 0xFF42: scy_ = value; return;
        case 0xFF43: scx_ = value; return;
        case 0xFF44: return;
        case 0xFF45:
            lyc_ = value;
            if (lcdc_ & 0x80) updateStat(irq);
            return;
        case 0xFF47: bgp_ = value; return;
        case 0xFF48: obp_[0] = value; return;
        case 0xFF49: obp_[1] = value; return;
        case 0xFF4A: wy_ = value; return;
        case 0xFF4B: wx_ = value; return;
        default: break;
    }
    if (!color_) return;
    switch (address) {
        case 0xFF4F: vramBank = value & 1; break;
        case 0xFF68: bcps_ = value & 0xBF; break;
        case 0xFF69:
            bgPal_[bcps_ & 63] = value;
            if (bcps_ & 0x80) bcps_ = static_cast<uint8_t>(0x80 | ((bcps_ + 1) & 63));
            break;
        case 0xFF6A: ocps_ = value & 0xBF; break;
        case 0xFF6B:
            obPal_[ocps_ & 63] = value;
            if (ocps_ & 0x80) ocps_ = static_cast<uint8_t>(0x80 | ((ocps_ + 1) & 63));
            break;
        case 0xFF6C: opri_ = value & 1; break;
        default: break;
    }
}

void Ppu::blankFrame() {
    if (!drawing_ || !frame_) return;
    VideoFrame& f = *frame_;
    const uint32_t white = color_ ? 0xFFFFFFFFu : dmg_[0];
    for (int y = 0; y < kScreenHeight; ++y) {
        f.backdrop[static_cast<size_t>(y)] = white;
        f.lines[static_cast<size_t>(y)] = {0, 0, false};
        for (int x = 0; x < kScreenWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * VideoFrame::kMaxWidth + static_cast<size_t>(x);
            f.rgba[i] = white;
            f.info[i] = Pixel{};
            f.planeB[i] = f.planeA[i] = f.sprites[i] = 0;
            f.planeBInfo[i] = f.planeAInfo[i] = f.spriteInfo[i] = Pixel{};
        }
    }
}

void Ppu::renderLine() {
    const int y = line_;
    const bool bgOn = color_ || (lcdc_ & 1);
    const bool window = (lcdc_ & 0x20) && wyHit_ && wx_ <= 166 && bgOn;
    if (!drawing_ || !frame_) {
        if (window) ++winLine_;
        return;
    }
    VideoFrame& f = *frame_;
    // Sprites always go over the background when the Color's LCDC bit 0 is off.
    const bool bgPriority = !color_ || (lcdc_ & 1);
    const int winStart = wx_ - 7;

    struct Bg {
        int index = 0, palette = 0;
        bool priority = false;
        uint16_t source = 0;
    };
    auto fetch = [&](int map, int x, int yy) {
        Bg b;
        const int m = map + ((yy >> 3) & 31) * 32 + ((x >> 3) & 31);
        const uint8_t tile = vram[0][m];
        const uint8_t attr = color_ ? vram[1][m] : 0;
        const int address = (lcdc_ & 0x10) ? tile * 16 : 0x1000 + static_cast<int8_t>(tile) * 16;
        int row = yy & 7, col = x & 7;
        if (attr & 0x40) row = 7 - row;
        if (attr & 0x20) col = 7 - col;
        const uint8_t* bank = vram[(attr >> 3) & 1];
        const int lo = bank[address + row * 2], hi = bank[address + row * 2 + 1], bit = 7 - col;
        b.index = ((lo >> bit) & 1) | ((hi >> bit) & 1) << 1;
        b.palette = attr & 7;
        b.priority = attr & 0x80;
        b.source = static_cast<uint16_t>(address / 16 | (attr & 8) << 6);
        return b;
    };
    auto bgColour = [&](const Bg& b) {
        if (color_) return cgbColour(bgPal_, b.palette, b.index);
        return bgOn ? dmg_[(bgp_ >> (b.index * 2)) & 3] : dmg_[0];
    };

    // The sprites of the line, the one drawn first winning each pixel.
    int8_t sprite[kScreenWidth];
    uint8_t sprIndex[kScreenWidth], sprAttr[kScreenWidth];
    std::memset(sprite, -1, sizeof sprite);
    if (lcdc_ & 2) {
        const int height = lcdc_ & 4 ? 16 : 8;
        for (int s = 0; s < spriteCount_; ++s) {
            const uint8_t* o = &oam[sprites_[s] * 4];
            const int sy = o[0] - 16, sx = o[1] - 8, attr = o[3];
            int tile = o[2];
            int row = y - sy;
            if (row < 0 || row >= height) continue;
            if (attr & 0x40) row = height - 1 - row;
            if (height == 16) tile &= 0xFE;
            const uint8_t* bank = vram[color_ ? (attr >> 3) & 1 : 0];
            const int lo = bank[tile * 16 + row * 2], hi = bank[tile * 16 + row * 2 + 1];
            for (int px = 0; px < 8; ++px) {
                const int x = sx + px;
                if (x < 0 || x >= kScreenWidth || sprite[x] >= 0) continue;
                const int bit = attr & 0x20 ? px : 7 - px;
                const int index = ((lo >> bit) & 1) | ((hi >> bit) & 1) << 1;
                if (!index) continue;
                sprite[x] = static_cast<int8_t>(sprites_[s]);
                sprIndex[x] = static_cast<uint8_t>(index);
                sprAttr[x] = static_cast<uint8_t>(attr);
            }
        }
    }

    const uint32_t backdrop = color_ ? cgbColour(bgPal_, 0, 0) : bgOn ? dmg_[bgp_ & 3] : dmg_[0];
    f.backdrop[static_cast<size_t>(y)] = backdrop;
    if (y == kScreenHeight / 2) unwrappedScx_ += static_cast<int8_t>(scx_ - static_cast<uint8_t>(unwrappedScx_));
    const int16_t scroll =
        static_cast<int16_t>(-(unwrappedScx_ + static_cast<int8_t>(scx_ - static_cast<uint8_t>(unwrappedScx_))));
    f.lines[static_cast<size_t>(y)] = {scroll, scroll, true};

    const int bgMap = lcdc_ & 8 ? 0x1C00 : 0x1800, winMap = lcdc_ & 0x40 ? 0x1C00 : 0x1800;
    for (int x = 0; x < kScreenWidth; ++x) {
        const size_t i = static_cast<size_t>(y) * VideoFrame::kMaxWidth + static_cast<size_t>(x);
        Bg b = fetch(bgMap, x + scx_, y + scy_);
        if (!bgOn) b = Bg{};
        const uint32_t bc = bgColour(b);
        const bool clear = b.index == 0 && bc == backdrop;
        f.planeB[i] = clear ? 0 : bc;
        const Pixel bgPixel{static_cast<uint8_t>(b.palette * 16 + b.index), Layer::Back, b.priority, Shade::Normal,
                            b.source};
        f.planeBInfo[i] = bgPixel;

        Bg top = b;
        uint32_t topColour = bc;
        Pixel topPixel = clear ? Pixel{} : bgPixel;
        if (window && x >= winStart) {
            const Bg w = fetch(winMap, x - winStart, winLine_);
            const uint32_t wc = bgColour(w);
            f.planeA[i] = wc;
            f.planeAInfo[i] = {static_cast<uint8_t>(w.palette * 16 + w.index), Layer::Window, w.priority,
                               Shade::Normal, w.source};
            top = w;
            topColour = wc;
            topPixel = f.planeAInfo[i];
        } else {
            f.planeA[i] = 0;
            f.planeAInfo[i] = Pixel{};
        }

        if (sprite[x] >= 0) {
            const int attr = sprAttr[x], index = sprIndex[x];
            const int palette = color_ ? attr & 7 : (attr >> 4) & 1;
            const uint32_t sc =
                color_ ? cgbColour(obPal_, palette, index) : dmg_[(obp_[palette] >> (index * 2)) & 3];
            const bool behind = attr & 0x80;
            f.sprites[i] = sc;
            f.spriteInfo[i] = {static_cast<uint8_t>((8 + palette) * 16 + index), Layer::Sprite, !behind,
                               Shade::Normal, static_cast<uint16_t>(sprite[x])};
            const bool over = !bgPriority || top.index == 0 || (!behind && !top.priority);
            if (over) {
                topColour = sc;
                topPixel = f.spriteInfo[i];
            }
        } else {
            f.sprites[i] = 0;
            f.spriteInfo[i] = Pixel{};
        }
        f.rgba[i] = topColour;
        f.info[i] = topPixel;
    }
    if (window) ++winLine_;
}

}  // namespace plumnin::gb
