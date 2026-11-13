#include "snes_ppu.h"

#include <algorithm>

namespace plumnin::snes {

namespace {

// Bits per pixel of each background in each mode (0: no such background).
constexpr int kBpp[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
                            {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}};

// Sprite sizes for each OBSEL setting: small and large, width and height.
constexpr int kObjWidth[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
constexpr int kObjHeight[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {32, 64}, {32, 32}};

constexpr int kObj = 4, kBackdrop = 5;

// How far in front each layer is, by mode: rank[layer][priority], higher in front.
// Backgrounds have priorities 0 and 1, sprites 0 to 3; 0 is never drawn.
struct Ranks {
    int bg[4][2];
    int obj[4];
};

Ranks ranksFor(int mode, bool bg3High) {
    // Lists from front to back, as fullsnes gives them: "S3 1H 2H S2..." as (layer, priority).
    struct Entry {
        int layer, priority;
    };
    static const Entry mode0[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {3, 1}, {4, 0}, {2, 0}, {3, 0}};
    static const Entry mode1[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {4, 0}, {2, 0}};
    static const Entry mode1High[] = {{2, 1}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {4, 0}, {2, 0}};
    static const Entry mode2[] = {{4, 3}, {0, 1}, {4, 2}, {1, 1}, {4, 1}, {0, 0}, {4, 0}, {1, 0}};
    static const Entry mode6[] = {{4, 3}, {0, 1}, {4, 2}, {4, 1}, {0, 0}, {4, 0}};
    static const Entry mode7[] = {{4, 3}, {4, 2}, {1, 1}, {4, 1}, {0, 0}, {4, 0}, {1, 0}};
    const Entry* list;
    int n;
    switch (mode) {
        case 0: list = mode0, n = 12; break;
        case 1: list = bg3High ? mode1High : mode1, n = 10; break;
        case 6: list = mode6, n = 6; break;
        case 7: list = mode7, n = 7; break;
        default: list = mode2, n = 8; break;
    }
    Ranks r{};
    for (int i = 0; i < n; ++i) {
        const int rank = n - i;
        if (list[i].layer == kObj) r.obj[list[i].priority] = rank;
        else r.bg[list[i].layer][list[i].priority] = rank;
    }
    // Mode 7's first background has no priority bit: both levels are its one rank.
    if (mode == 7) r.bg[0][1] = r.bg[0][0];
    return r;
}

int sign13(int v) { return static_cast<int16_t>(static_cast<uint16_t>(v << 3)) >> 3; }

}  // namespace

Ppu::Ppu() {
    for (uint32_t c = 0; c < rgb_.size(); ++c) {
        const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        rgb_[c] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
}

void Ppu::power() {
    vram_.fill(0);
    cgram_.fill(0);
    oam_.fill(0);
    highOam_.fill(0);
    reset();
}

void Ppu::reset() {
    inidisp_ = 0x80;
    obsel_ = 0;
    oamAddr_ = oamBase_ = 0;
    oamPriority_ = false;
    oamLatch_ = 0;
    bgmode_ = mosaic_ = 0;
    bgsc_.fill(0);
    bgnba_.fill(0);
    hofs_.fill(0);
    vofs_.fill(0);
    ofsLatch_ = m7Latch_ = 0;
    vmain_ = 0;
    vmaddr_ = vramLatch_ = 0;
    m7sel_ = 0;
    m7_.fill(0);
    m7x_ = m7y_ = m7hofs_ = m7vofs_ = 0;
    cgaddr_ = 0;
    cgHigh_ = false;
    cgLatch_ = 0;
    w12sel_.fill(0);
    wobjsel_ = 0;
    wh_.fill(0);
    wbglog_ = wobjlog_ = tm_ = ts_ = tmw_ = tsw_ = cgwsel_ = cgadsub_ = 0;
    fixed_.fill(0);
    setini_ = 0;
    hcounter_ = vcounter_ = 0;
    hFlip_ = vFlip_ = latched_ = false;
    timeOver_ = rangeOver_ = field_ = false;
    ppu1Bus_ = ppu2Bus_ = 0;
    mosaicLine_ = 1;
    hires_ = false;
}

uint16_t Ppu::vramAddress() const {
    const uint16_t a = vmaddr_;
    switch ((vmain_ >> 2) & 3) {
        case 1: return static_cast<uint16_t>((a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7)) & 0x7FFF;
        case 2: return static_cast<uint16_t>((a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7)) & 0x7FFF;
        case 3: return static_cast<uint16_t>((a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7)) & 0x7FFF;
        default: return a & 0x7FFF;
    }
}

void Ppu::vramPrefetch() { vramLatch_ = vram_[vramAddress()]; }

void Ppu::bumpVram(bool high) {
    if (high != static_cast<bool>(vmain_ & 0x80)) return;
    static const uint16_t kSteps[4] = {1, 32, 128, 128};
    vmaddr_ = static_cast<uint16_t>(vmaddr_ + kSteps[vmain_ & 3]);
}

void Ppu::writeOam(uint8_t v) {
    const uint16_t a = oamAddr_;
    if (a & 0x200) {
        highOam_[a & 0x1F] = v;
    } else if (!(a & 1)) {
        oamLatch_ = v;
    } else {
        oam_[a - 1] = oamLatch_;
        oam_[a] = v;
    }
    oamAddr_ = (a + 1) & 0x3FF;
}

uint8_t Ppu::readOam() {
    const uint16_t a = oamAddr_;
    const uint8_t v = a & 0x200 ? highOam_[a & 0x1F] : oam_[a];
    oamAddr_ = (a + 1) & 0x3FF;
    return v;
}

void Ppu::latchCounters() {
    hcounter_ = static_cast<uint16_t>(dot_);
    vcounter_ = static_cast<uint16_t>(line_);
    latched_ = true;
}

void Ppu::write(uint8_t reg, uint8_t v) {
    switch (reg) {
        case 0x00:
            // Leaving forced blank in vblank puts the OAM address back where it was set.
            if ((inidisp_ & 0x80) && !(v & 0x80) && line_ > visibleLines()) oamAddr_ = static_cast<uint16_t>(oamBase_ << 1);
            inidisp_ = v;
            break;
        case 0x01: obsel_ = v; break;
        case 0x02:
            oamBase_ = static_cast<uint16_t>((oamBase_ & 0x100) | v);
            oamAddr_ = static_cast<uint16_t>(oamBase_ << 1);
            break;
        case 0x03:
            oamBase_ = static_cast<uint16_t>((oamBase_ & 0xFF) | (v & 1) << 8);
            oamPriority_ = v & 0x80;
            oamAddr_ = static_cast<uint16_t>(oamBase_ << 1);
            break;
        case 0x04: writeOam(v); break;
        case 0x05: bgmode_ = v; break;
        case 0x06:
            mosaic_ = v;
            mosaicLine_ = static_cast<int16_t>(line_ < 1 || line_ > visibleLines() ? 1 : line_);
            break;
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0A: bgsc_[reg - 0x07] = v; break;
        case 0x0B:
            bgnba_[0] = v & 0x0F;
            bgnba_[1] = v >> 4;
            break;
        case 0x0C:
            bgnba_[2] = v & 0x0F;
            bgnba_[3] = v >> 4;
            break;
        case 0x0D:
        case 0x0F:
        case 0x11:
        case 0x13: {
            const int bg = (reg - 0x0D) >> 1;
            if (reg == 0x0D) {
                m7hofs_ = static_cast<int16_t>(sign13(v << 8 | m7Latch_));
                m7Latch_ = v;
            }
            hofs_[bg] = static_cast<uint16_t>((v << 8) | (ofsLatch_ & ~7) | ((hofs_[bg] >> 8) & 7));  // 10 bits used
            ofsLatch_ = v;
            break;
        }
        case 0x0E:
        case 0x10:
        case 0x12:
        case 0x14: {
            const int bg = (reg - 0x0E) >> 1;
            if (reg == 0x0E) {
                m7vofs_ = static_cast<int16_t>(sign13(v << 8 | m7Latch_));
                m7Latch_ = v;
            }
            vofs_[bg] = static_cast<uint16_t>((v << 8) | ofsLatch_);
            ofsLatch_ = v;
            break;
        }
        case 0x15: vmain_ = v; break;
        case 0x16:
            vmaddr_ = static_cast<uint16_t>((vmaddr_ & 0xFF00) | v);
            vramPrefetch();
            break;
        case 0x17:
            vmaddr_ = static_cast<uint16_t>((vmaddr_ & 0x00FF) | v << 8);
            vramPrefetch();
            break;
        case 0x18:
        case 0x19: {
            // VRAM can only be written in vblank or forced blank.
            if (forceBlank() || line_ > visibleLines() || line_ == 0) {
                uint16_t& w = vram_[vramAddress()];
                w = reg == 0x18 ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | v << 8);
            }
            bumpVram(reg == 0x19);
            break;
        }
        case 0x1A: m7sel_ = v; break;
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
            m7_[reg - 0x1B] = static_cast<int16_t>(v << 8 | m7Latch_);
            m7Latch_ = v;
            break;
        case 0x1F:
            m7x_ = static_cast<int16_t>(sign13(v << 8 | m7Latch_));
            m7Latch_ = v;
            break;
        case 0x20:
            m7y_ = static_cast<int16_t>(sign13(v << 8 | m7Latch_));
            m7Latch_ = v;
            break;
        case 0x21:
            cgaddr_ = v;
            cgHigh_ = false;
            break;
        case 0x22:
            if (!cgHigh_) {
                cgLatch_ = v;
            } else {
                cgram_[cgaddr_] = static_cast<uint16_t>((v & 0x7F) << 8 | cgLatch_);
                ++cgaddr_;
            }
            cgHigh_ = !cgHigh_;
            break;
        case 0x23: w12sel_[0] = v; break;
        case 0x24: w12sel_[1] = v; break;
        case 0x25: wobjsel_ = v; break;
        case 0x26:
        case 0x27:
        case 0x28:
        case 0x29: wh_[reg - 0x26] = v; break;
        case 0x2A: wbglog_ = v; break;
        case 0x2B: wobjlog_ = v; break;
        case 0x2C: tm_ = v; break;
        case 0x2D: ts_ = v; break;
        case 0x2E: tmw_ = v; break;
        case 0x2F: tsw_ = v; break;
        case 0x30: cgwsel_ = v; break;
        case 0x31: cgadsub_ = v; break;
        case 0x32:
            if (v & 0x20) fixed_[0] = v & 0x1F;
            if (v & 0x40) fixed_[1] = v & 0x1F;
            if (v & 0x80) fixed_[2] = v & 0x1F;
            break;
        case 0x33: setini_ = v; break;
        default: break;
    }
}

uint8_t Ppu::read(uint8_t reg, uint8_t mdr) {
    switch (reg) {
        case 0x34:
        case 0x35:
        case 0x36: {
            const int32_t product = m7_[0] * static_cast<int8_t>(m7_[1] >> 8);
            ppu1Bus_ = static_cast<uint8_t>(product >> ((reg - 0x34) * 8));
            return ppu1Bus_;
        }
        case 0x37: latchCounters(); return mdr;
        case 0x38: return ppu1Bus_ = readOam();
        case 0x39:
        case 0x3A: {
            ppu1Bus_ = static_cast<uint8_t>(reg == 0x39 ? vramLatch_ : vramLatch_ >> 8);
            if ((reg == 0x3A) == static_cast<bool>(vmain_ & 0x80)) {
                vramPrefetch();
                bumpVram(reg == 0x3A);
            }
            return ppu1Bus_;
        }
        case 0x3B: {
            const uint16_t c = cgram_[cgaddr_];
            if (!cgHigh_) {
                ppu2Bus_ = static_cast<uint8_t>(c);
            } else {
                ppu2Bus_ = static_cast<uint8_t>((ppu2Bus_ & 0x80) | ((c >> 8) & 0x7F));
                ++cgaddr_;
            }
            cgHigh_ = !cgHigh_;
            return ppu2Bus_;
        }
        case 0x3C:
            ppu2Bus_ = hFlip_ ? static_cast<uint8_t>((ppu2Bus_ & 0xFE) | ((hcounter_ >> 8) & 1)) : static_cast<uint8_t>(hcounter_);
            hFlip_ = !hFlip_;
            return ppu2Bus_;
        case 0x3D:
            ppu2Bus_ = vFlip_ ? static_cast<uint8_t>((ppu2Bus_ & 0xFE) | ((vcounter_ >> 8) & 1)) : static_cast<uint8_t>(vcounter_);
            vFlip_ = !vFlip_;
            return ppu2Bus_;
        case 0x3E:
            ppu1Bus_ = static_cast<uint8_t>((timeOver_ ? 0x80 : 0) | (rangeOver_ ? 0x40 : 0) | (ppu1Bus_ & 0x10) | 0x01);
            return ppu1Bus_;
        case 0x3F:
            ppu2Bus_ = static_cast<uint8_t>((field_ ? 0x80 : 0) | (latched_ ? 0x40 : 0) | (ppu2Bus_ & 0x20) | (pal_ ? 0x10 : 0) | 0x03);
            hFlip_ = vFlip_ = false;
            latched_ = false;
            return ppu2Bus_;
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x28:
        case 0x29:
        case 0x2A: return ppu1Bus_;
        default: return mdr;
    }
}

void Ppu::startFrame(bool pal) {
    pal_ = pal;
    hires_ = false;
    mosaicLine_ = 1;
    field_ = !field_;
    if (!forceBlank()) timeOver_ = rangeOver_ = false;
    if (frame_) {
        frame_->width = 256;
        frame_->wideLeft = frame_->wideRight = 0;
    }
}

void Ppu::startVblank() {
    if (!forceBlank()) oamAddr_ = static_cast<uint16_t>(oamBase_ << 1);
    if (frame_) {
        frame_->height = visibleLines();
        frame_->aspectLines = visibleLines();
    }
}

void Ppu::drawBackground(int bg, int y, int width, LayerLine& out) {
    const int mode = bgmode_ & 7;
    const int bpp = kBpp[mode][bg];
    const bool hiresMode = mode == 5 || mode == 6;
    const bool big = bgmode_ & (0x10 << bg);
    const int tileH = big ? 16 : 8;
    const int tileW = hiresMode ? 16 : tileH;
    const int mosaicSize = (mosaic_ & (1 << bg)) ? (mosaic_ >> 4) + 1 : 1;
    if (mosaicSize > 1) y = mosaicLine_ + (y - mosaicLine_) / mosaicSize * mosaicSize;

    const uint8_t sc = bgsc_[bg];
    const uint16_t mapBase = static_cast<uint16_t>((sc & 0xFC) << 8);
    const int mapW = sc & 1 ? 64 : 32, mapH = sc & 2 ? 64 : 32;
    const uint16_t charBase = static_cast<uint16_t>(bgnba_[bg] << 12);
    const int wordsPerTile = bpp * 4;
    const int paletteBase = mode == 0 ? bg * 32 : 0;
    const bool direct = bpp == 8 && (cgwsel_ & 1);
    const bool opt = (mode == 2 || mode == 4 || mode == 6) && bg < 2;

    auto mapEntry = [&](uint16_t base, int w, int h, int tx, int ty) {
        tx &= w - 1;
        ty &= h - 1;
        const int screen = (tx >> 5) + (ty >> 5) * (w == 64 ? 2 : 1);
        return vram_[(base + screen * 0x400 + (ty & 31) * 32 + (tx & 31)) & 0x7FFF];
    };

    int cachedTile = -1, cachedRow = -1;
    uint16_t planes[4] = {};
    for (int x = 0; x < width; ++x) {
        int hs = hofs_[bg] & 0x3FF, vs = vofs_[bg] & 0x3FF;
        if (hiresMode) hs <<= 1;
        if (opt) {
            // Offset per tile: BG3's map gives each column of tiles (but the first) its own scroll.
            const int screenX = hiresMode ? x >> 1 : x;
            const int col = (screenX + (hofs_[bg] & 7)) >> 3;
            if (col > 0) {
                const uint8_t sc3 = bgsc_[2];
                const uint16_t base3 = static_cast<uint16_t>((sc3 & 0xFC) << 8);
                const int w3 = sc3 & 1 ? 64 : 32, h3 = sc3 & 2 ? 64 : 32;
                const int tx = (col - 1) + ((hofs_[2] & 0x3FF) >> 3);
                const int ty = (vofs_[2] & 0x3FF) >> 3;
                const uint16_t enable = static_cast<uint16_t>(0x2000 << bg);
                uint16_t hval = mapEntry(base3, w3, h3, tx, ty), vval = 0;
                bool useH = false, useV = false;
                if (mode == 4) {
                    if (hval & 0x8000) {
                        vval = hval;
                        useV = vval & enable;
                    } else {
                        useH = hval & enable;
                    }
                } else {
                    vval = mapEntry(base3, w3, h3, tx, ty + 1);
                    useH = hval & enable;
                    useV = vval & enable;
                }
                if (useH) hs = ((hval & 0x3F8) | (hofs_[bg] & 7)) << (hiresMode ? 1 : 0);
                if (useV) vs = vval & 0x3FF;
            }
        }
        const int px = x + hs, py = y + vs;
        const int tx = px / tileW, ty = py / tileH;
        const uint16_t entry = mapEntry(mapBase, mapW, mapH, tx, ty);
        int fx = px % tileW, fy = py % tileH;
        if (entry & 0x4000) fx = tileW - 1 - fx;
        if (entry & 0x8000) fy = tileH - 1 - fy;
        const int tile = (entry & 0x3FF) + (fx >= 8 ? 1 : 0) + (fy >= 8 ? 16 : 0);
        fx &= 7;
        fy &= 7;
        if (tile != cachedTile || fy != cachedRow) {
            const uint16_t at = static_cast<uint16_t>(charBase + (tile & 0x3FF) * wordsPerTile);
            for (int p = 0; p < bpp / 2; ++p) planes[p] = vram_[(at + p * 8 + fy) & 0x7FFF];
            cachedTile = tile;
            cachedRow = fy;
        }
        int c = 0;
        for (int p = 0; p < bpp / 2; ++p) {
            const uint16_t w = planes[p];
            c |= (((w >> (7 - fx)) & 1) | ((w >> (15 - fx)) & 1) << 1) << (2 * p);
        }
        if (!c) {
            out.index[x] = 0;
            continue;
        }
        const int pal = (entry >> 10) & 7;
        int index;
        uint16_t color;
        if (bpp == 8) {
            index = c;
            if (direct) {
                color = static_cast<uint16_t>(((c & 7) << 2 | (pal & 1) << 1) | (((c >> 3) & 7) << 2 | (pal & 2)) << 5 |
                                              (((c >> 6) & 3) << 3 | (pal & 4)) << 10);
            } else {
                color = paletteColor(index);
            }
        } else {
            index = paletteBase + pal * (bpp == 2 ? 4 : 16) + c;
            color = paletteColor(index);
        }
        out.index[x] = static_cast<uint16_t>(index + 1);
        out.color[x] = color;
        out.priority[x] = static_cast<uint8_t>((entry >> 13) & 1);
        out.source[x] = static_cast<uint16_t>(entry & 0x3FF);
    }
    // Horizontal mosaic: each block shows its first pixel.
    if (mosaicSize > 1) {
        const int step = hiresMode ? mosaicSize * 2 : mosaicSize;
        for (int x = 0; x < width; ++x) {
            const int from = x - x % step;
            if (from == x) continue;
            out.index[x] = out.index[from];
            out.color[x] = out.color[from];
            out.priority[x] = out.priority[from];
            out.source[x] = out.source[from];
        }
    }
}

void Ppu::drawMode7(int y, LayerLine& bg1, LayerLine& bg2, bool extbg) {
    const int mosaicSize = (mosaic_ & 1) ? (mosaic_ >> 4) + 1 : 1;
    if (mosaicSize > 1) y = mosaicLine_ + (y - mosaicLine_) / mosaicSize * mosaicSize;
    auto clip = [](int n) { return n & 0x2000 ? (n | ~0x3FF) : (n & 0x3FF); };
    const int a = m7_[0], b = m7_[1], c = m7_[2], d = m7_[3];
    const int hofs = m7hofs_, vofs = m7vofs_, cx = m7x_, cy = m7y_;
    const int yy = (m7sel_ & 2) ? 255 - y : y;
    const int psx = ((a * clip(hofs - cx)) & ~63) + ((b * clip(vofs - cy)) & ~63) + ((b * yy) & ~63) + (cx << 8);
    const int psy = ((c * clip(hofs - cx)) & ~63) + ((d * clip(vofs - cy)) & ~63) + ((d * yy) & ~63) + (cy << 8);
    const int repeat = m7sel_ >> 6;
    const bool direct = cgwsel_ & 1;
    for (int x = 0; x < 256; ++x) {
        const int xx = (m7sel_ & 1) ? 255 - x : x;
        const int px = (psx + a * xx) >> 8, py = (psy + c * xx) >> 8;
        const bool outside = (px | py) & ~1023;
        bg1.index[x] = bg2.index[x] = 0;
        if (outside && repeat == 2) continue;
        const int tile = outside && repeat == 3 ? 0 : vram_[((py >> 3) & 127) * 128 + ((px >> 3) & 127)] & 0xFF;
        const int pixel = vram_[(tile * 64 + (py & 7) * 8 + (px & 7)) & 0x7FFF] >> 8;
        if (pixel) {
            bg1.index[x] = static_cast<uint16_t>(pixel + 1);
            bg1.color[x] = direct ? static_cast<uint16_t>((pixel & 7) << 2 | ((pixel >> 3) & 7) << 7 | ((pixel >> 6) & 3) << 13)
                                  : paletteColor(pixel);
            bg1.priority[x] = 0;
            bg1.source[x] = static_cast<uint16_t>(tile);
        }
        if (extbg && (pixel & 0x7F)) {
            bg2.index[x] = static_cast<uint16_t>((pixel & 0x7F) + 1);
            bg2.color[x] = paletteColor(pixel & 0x7F);
            bg2.priority[x] = static_cast<uint8_t>(pixel >> 7);
            bg2.source[x] = static_cast<uint16_t>(tile);
        }
    }
    if (mosaicSize > 1) {
        for (LayerLine* l : {&bg1, &bg2})
            for (int x = 0; x < 256; ++x) {
                const int from = x - x % mosaicSize;
                if (from == x) continue;
                l->index[x] = l->index[from];
                l->color[x] = l->color[from];
                l->priority[x] = l->priority[from];
                l->source[x] = l->source[from];
            }
    }
}

void Ppu::drawSprites(int y, LayerLine& out) {
    std::fill(out.index.begin(), out.index.begin() + 256, uint16_t{0});
    const int row = y - 1;
    const int sizeSel = obsel_ >> 5;
    const int first = oamPriority_ ? (oamBase_ >> 1) & 0x7F : 0;
    int list[128];
    int count = 0;
    for (int n = 0; n < 128; ++n) {
        const int i = (first + n) & 0x7F;
        const uint8_t high = static_cast<uint8_t>(highOam_[i >> 2] >> ((i & 3) * 2));
        const int large = (high >> 1) & 1;
        const int w = kObjWidth[sizeSel][large], h = kObjHeight[sizeSel][large];
        const int x = oam_[i * 4] | (high & 1) << 8;
        const int sy = oam_[i * 4 + 1];
        if (((row - sy) & 0xFF) >= h) continue;
        if (x >= 256 && x + w <= 512) continue;  // off the left edge entirely
        if (count == 32) {
            rangeOver_ = true;
            if (!unlimited_) break;
        }
        list[count++] = i;
    }
    const uint16_t nameBase = static_cast<uint16_t>((obsel_ & 7) << 13);
    const uint16_t nameSelect = static_cast<uint16_t>((((obsel_ >> 3) & 3) + 1) << 12);
    int tiles = 0;
    // The tiles are fetched from the last sprite in range to the first: past 34, the
    // first ones lose theirs. Drawn in that order, the first sprite ends on top.
    for (int k = count - 1; k >= 0; --k) {
        const int i = list[k];
        const uint8_t high = static_cast<uint8_t>(highOam_[i >> 2] >> ((i & 3) * 2));
        const int large = (high >> 1) & 1;
        const int w = kObjWidth[sizeSel][large], h = kObjHeight[sizeSel][large];
        int x = oam_[i * 4] | (high & 1) << 8;
        if (x >= 256) x -= 512;
        const int sy = oam_[i * 4 + 1];
        const uint8_t chr = oam_[i * 4 + 2], attr = oam_[i * 4 + 3];
        int fy = (row - sy) & 0xFF;
        if (attr & 0x80) fy = h - 1 - fy;
        const int pal = (attr >> 1) & 7;
        const uint16_t base = static_cast<uint16_t>(attr & 1 ? nameBase + nameSelect : nameBase);
        const int columns = w / 8;
        for (int c = 0; c < columns; ++c) {
            const int tx = x + c * 8;
            if (tx <= -8 || tx >= 256) continue;
            if (++tiles > 34) {
                timeOver_ = true;
                if (!unlimited_) return;
            }
            const int col = attr & 0x40 ? columns - 1 - c : c;
            const int index = (((chr & 0xF0) + ((fy >> 3) << 4)) & 0xF0) | ((chr + col) & 0x0F);
            const uint16_t at = static_cast<uint16_t>((base + index * 16 + (fy & 7)) & 0x7FFF);
            const uint16_t p01 = vram_[at], p23 = vram_[(at + 8) & 0x7FFF];
            for (int p = 0; p < 8; ++p) {
                const int sx = tx + p;
                if (sx < 0 || sx >= 256) continue;
                const int bit = attr & 0x40 ? p : 7 - p;
                const int v = ((p01 >> bit) & 1) | ((p01 >> (bit + 8)) & 1) << 1 | ((p23 >> bit) & 1) << 2 |
                              ((p23 >> (bit + 8)) & 1) << 3;
                if (!v) continue;
                const int ci = 128 + pal * 16 + v;
                out.index[sx] = static_cast<uint16_t>(ci + 1);
                out.color[sx] = paletteColor(ci);
                out.priority[sx] = static_cast<uint8_t>((attr >> 4) & 3);
                out.math[sx] = pal >= 4;
                out.source[sx] = static_cast<uint16_t>(i);
            }
        }
    }
}

void Ppu::windowMask(int layer, std::array<bool, 256>& mask) const {
    int sel, logic;
    if (layer < 4) {
        sel = (w12sel_[layer >> 1] >> ((layer & 1) * 4)) & 0xF;
        logic = (wbglog_ >> (layer * 2)) & 3;
    } else if (layer == kObj) {
        sel = wobjsel_ & 0xF;
        logic = wobjlog_ & 3;
    } else {
        sel = wobjsel_ >> 4;
        logic = (wobjlog_ >> 2) & 3;
    }
    const bool e1 = sel & 2, e2 = sel & 8, i1 = sel & 1, i2 = sel & 4;
    for (int x = 0; x < 256; ++x) {
        const bool w1 = (x >= wh_[0] && x <= wh_[1]) != i1;
        const bool w2 = (x >= wh_[2] && x <= wh_[3]) != i2;
        bool in;
        if (e1 && e2) {
            switch (logic) {
                case 0: in = w1 || w2; break;
                case 1: in = w1 && w2; break;
                case 2: in = w1 != w2; break;
                default: in = w1 == w2; break;
            }
        } else if (e1) {
            in = w1;
        } else if (e2) {
            in = w2;
        } else {
            in = false;
        }
        mask[static_cast<size_t>(x)] = in;
    }
}

void Ppu::expandToHires() {
    VideoFrame& f = *frame_;
    for (int row = 0; row < f.height && row < VideoFrame::kMaxHeight; ++row) {
        const size_t base = static_cast<size_t>(row) * VideoFrame::kMaxWidth;
        for (int x = 255; x >= 0; --x) {
            const size_t from = base + static_cast<size_t>(x), to = base + static_cast<size_t>(x) * 2;
            for (auto* a : {&f.rgba, &f.planeB, &f.planeA, &f.sprites}) (*a)[to] = (*a)[to + 1] = (*a)[from];
            for (auto* a : {&f.info, &f.planeBInfo, &f.planeAInfo, &f.spriteInfo}) (*a)[to] = (*a)[to + 1] = (*a)[from];
        }
    }
    f.width = 512;
}

void Ppu::drawLine(int line, bool drawing) {
    if (!frame_ || line < 1 || line > visibleLines()) return;
    const int mode = bgmode_ & 7;
    const bool hiresLine = mode == 5 || mode == 6 || (setini_ & 0x08);
    if (!drawing) {
        if (hiresLine) hires_ = true;
        return;
    }
    if (hiresLine && !hires_) {
        frame_->height = std::max(frame_->height, line - 1);
        expandToHires();
        hires_ = true;
    }
    const int bgWidth = mode == 5 || mode == 6 ? 512 : 256;
    for (int bg = 0; bg < 4; ++bg) std::fill(bg_[static_cast<size_t>(bg)].index.begin(), bg_[static_cast<size_t>(bg)].index.end(), uint16_t{0});
    if (!forceBlank()) {
        if (mode == 7) {
            drawMode7(line, bg_[0], bg_[1], setini_ & 0x40);
        } else {
            for (int bg = 0; bg < 4; ++bg)
                if (kBpp[mode][bg] && ((tm_ | ts_) & (1 << bg))) drawBackground(bg, line, bgWidth, bg_[static_cast<size_t>(bg)]);
        }
        drawSprites(line, obj_);
    } else {
        std::fill(obj_.index.begin(), obj_.index.end(), uint16_t{0});
    }
    output(line - 1, bgWidth, hiresLine);
}

void Ppu::output(int row, int bgWidth, bool hiresLine) {
    VideoFrame& f = *frame_;
    const int mode = bgmode_ & 7;
    const bool bg3High = mode == 1 && (bgmode_ & 0x08);
    const Ranks ranks = ranksFor(mode, bg3High);
    const int brightness = inidisp_ & 0x0F;
    const bool blank = forceBlank();

    std::array<std::array<bool, 256>, 6> windows;
    for (int l = 0; l < 6; ++l) windowMask(l, windows[static_cast<size_t>(l)]);

    // Which backgrounds make the front plane of the 3D view: the first (and in mode 1
    // BG3 when it is put on top, the status bar most games make of it), or in mode 7 the
    // second, EXTBG's.
    auto frontPlane = [&](int bg) {
        if (mode == 7) return bg == 1;
        return bg == 0 || (bg3High && bg == 2);
    };

    struct Pick {
        int layer = kBackdrop;
        uint16_t color = 0;
        int index = 0;
        bool priority = false;
        bool math = false;
        uint16_t source = 0;
    };
    // The topmost pixel of a screen at column x (256 space) and hx (the backgrounds' space).
    auto pick = [&](uint8_t enable, uint8_t windowed, int x, int hx, uint8_t only) {
        Pick p;
        int best = 0;
        for (int bg = 0; bg < 4; ++bg) {
            if (!(enable & only & (1 << bg))) continue;
            const LayerLine& l = bg_[static_cast<size_t>(bg)];
            if (!l.index[static_cast<size_t>(hx)]) continue;
            if ((windowed & (1 << bg)) && windows[static_cast<size_t>(bg)][static_cast<size_t>(x)]) continue;
            const int r = ranks.bg[bg][l.priority[static_cast<size_t>(hx)] & 1];
            if (r > best) {
                best = r;
                p = {bg, l.color[static_cast<size_t>(hx)], l.index[static_cast<size_t>(hx)] - 1, l.priority[static_cast<size_t>(hx)] != 0,
                     static_cast<bool>(cgadsub_ & (1 << bg)), l.source[static_cast<size_t>(hx)]};
            }
        }
        if ((enable & only & 0x10) && obj_.index[static_cast<size_t>(x)] &&
            !((windowed & 0x10) && windows[kObj][static_cast<size_t>(x)])) {
            const int r = ranks.obj[obj_.priority[static_cast<size_t>(x)] & 3];
            if (r > best) {
                best = r;
                p = {kObj, obj_.color[static_cast<size_t>(x)], obj_.index[static_cast<size_t>(x)] - 1,
                     obj_.priority[static_cast<size_t>(x)] >= 2, (cgadsub_ & 0x10) && obj_.math[static_cast<size_t>(x)],
                     obj_.source[static_cast<size_t>(x)]};
            }
        }
        return p;
    };

    auto bright = [&](uint16_t c) {
        if (blank) return uint32_t{0xFF000000u};
        if (brightness == 15) return rgb_[c & 0x7FFF];
        const int r = (c & 31) * (brightness + 1) >> 4, g = ((c >> 5) & 31) * (brightness + 1) >> 4,
                  b = ((c >> 10) & 31) * (brightness + 1) >> 4;
        return rgb_[static_cast<size_t>(r | g << 5 | b << 10)];
    };
    const uint16_t fixed = static_cast<uint16_t>(fixed_[0] | fixed_[1] << 5 | fixed_[2] << 10);

    // Colour math of a main screen pixel with what is under it.
    auto math = [&](const Pick& main, int x, int hxSub) {
        const bool inColorWindow = windows[kBackdrop][static_cast<size_t>(x)];
        auto region = [&](int sel) { return sel == 3 || (sel == 1 && !inColorWindow) || (sel == 2 && inColorWindow); };
        const bool clip = region(cgwsel_ >> 6);
        const bool prevent = region((cgwsel_ >> 4) & 3);
        uint16_t c = clip ? 0 : main.color;
        const bool on = main.layer == kBackdrop ? (cgadsub_ & 0x20) : main.math;
        if (prevent || !on) return c;
        uint16_t add = fixed;
        bool half = (cgadsub_ & 0x40) && !clip;
        if (cgwsel_ & 0x02) {
            const Pick sub = pick(ts_, tsw_, x, hxSub, 0x1F);
            if (sub.layer == kBackdrop) half = false;
            else add = sub.color;
        }
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        const int ar = add & 31, ag = (add >> 5) & 31, ab = (add >> 10) & 31;
        if (cgadsub_ & 0x80) {
            r = std::max(r - ar, 0), g = std::max(g - ag, 0), b = std::max(b - ab, 0);
            if (half) r >>= 1, g >>= 1, b >>= 1;
        } else {
            r += ar, g += ag, b += ab;
            if (half) r >>= 1, g >>= 1, b >>= 1;
            r = std::min(r, 31), g = std::min(g, 31), b = std::min(b, 31);
        }
        return static_cast<uint16_t>(r | g << 5 | b << 10);
    };

    auto layerOf = [&](int layer) {
        if (layer == kObj) return Layer::Sprite;
        if (layer == kBackdrop) return Layer::Backdrop;
        if (bg3High && layer == 2) return Layer::Window;
        return frontPlane(layer) ? Layer::Front : Layer::Back;
    };
    auto infoOf = [&](const Pick& p) {
        if (p.layer == kBackdrop) return Pixel{};
        return Pixel{static_cast<uint8_t>(p.index), layerOf(p.layer), p.priority, Shade::Normal, p.source};
    };

    const int outWidth = hires_ ? 512 : 256;
    const size_t base = static_cast<size_t>(row) * VideoFrame::kMaxWidth;
    const uint32_t backdrop = bright(cgram_[0]);
    f.backdrop[static_cast<size_t>(row)] = backdrop;
    f.lines[static_cast<size_t>(row)] = {static_cast<int16_t>(-(hofs_[0] & 0x3FF)), static_cast<int16_t>(-(hofs_[1] & 0x3FF)), !blank};
    for (int ox = 0; ox < outWidth; ++ox) {
        const int x = hires_ ? ox >> 1 : ox;
        // In high resolution the even columns are the sub screen's, the odd the main's.
        const bool subColumn = hires_ && hiresLine && !(ox & 1);
        const int hx = bgWidth == 512 ? (hires_ ? ox : ox * 2) : x;
        const int hxMain = bgWidth == 512 ? (hires_ ? (ox | 1) : ox * 2 + 1) : x;
        const size_t i = base + static_cast<size_t>(ox);
        uint16_t color;
        Pick top;
        if (subColumn) {
            top = pick(ts_, tsw_, x, hx, 0x1F);
            color = top.layer == kBackdrop ? fixed : top.color;
            top = pick(tm_, tmw_, x, hxMain, 0x1F);  // what the 3D view sees is the main screen
        } else {
            top = pick(tm_, tmw_, x, hxMain, 0x1F);
            if (top.layer == kBackdrop) top.color = cgram_[0] & 0x7FFF;
            color = math(top, x, hxMain);
        }
        f.rgba[i] = bright(color);
        f.info[i] = infoOf(top);

        // The planes, pulled apart for the 3D view.
        uint8_t back = 0, front = 0;
        for (int bg = 0; bg < 4; ++bg) (frontPlane(bg) ? front : back) |= static_cast<uint8_t>(1 << bg);
        const Pick b = pick(tm_, tmw_, x, hxMain, back);
        const Pick a = pick(tm_, tmw_, x, hxMain, front);
        const Pick s = pick(tm_, tmw_, x, hxMain, 0x10);
        f.planeB[i] = b.layer == kBackdrop ? 0 : bright(b.color);
        f.planeBInfo[i] = infoOf(b);
        f.planeA[i] = a.layer == kBackdrop ? 0 : bright(a.color);
        f.planeAInfo[i] = infoOf(a);
        f.sprites[i] = s.layer == kBackdrop ? 0 : bright(s.color);
        f.spriteInfo[i] = infoOf(s);
    }
    if (blank) {
        for (int ox = 0; ox < outWidth; ++ox) {
            const size_t i = base + static_cast<size_t>(ox);
            f.planeB[i] = f.planeA[i] = f.sprites[i] = 0;
            f.info[i] = f.planeBInfo[i] = f.planeAInfo[i] = f.spriteInfo[i] = Pixel{};
        }
    }
}

}  // namespace plumnin::snes
