#pragma once

#include <array>
#include <cstdint>

#include "../state_io.h"
#include "../video_frame.h"
#include "nes_cart.h"

namespace plumnin::nes {

constexpr int kDotsPerLine = 341;
// Of the 240 lines the PPU draws, a TV shows about 224: the frame leaves out the
// first and last 8.
constexpr int kFirstShownLine = 8, kShownLines = 224;

// The picture processor (2C02, 2C07 on PAL), one dot at a time: the background's
// shift registers, the sprite evaluation of the next line with its overflow bug, and
// the fetches the mappers watch on the address bus.
class Ppu {
public:
    Ppu();
    void setFrame(VideoFrame* f) { frame_ = f; }
    void setMapper(Mapper* m) { mapper_ = m; }
    void power(bool pal);
    void reset();
    void setDrawing(bool on) { drawing_ = on; }
    void setUnlimitedSprites(bool on) { unlimited_ = on; }

    void step();  // one dot
    uint8_t read(uint16_t reg);
    uint8_t peek(uint16_t reg) const;
    void write(uint16_t reg, uint8_t value);

    // The /NMI line: low (true here) while in vblank with NMIs on.
    bool nmi() const { return (status_ & 0x80) && (ctrl_ & 0x80); }
    bool takeFrameDone() {
        const bool d = frameDone_;
        frameDone_ = false;
        return d;
    }
    int scanline() const { return scanline_; }
    int dot() const { return dot_; }

    std::array<uint8_t, 0x1000> vram{};  // nametables (2 KB, or 4 KB with four-screen VRAM)
    std::array<uint8_t, 0x100> oam{};
    std::array<uint8_t, 0x20> palette{};

    template <class Ar>
    void state(Ar& ar) {
        ar(vram, oam, palette, secondary_, ctrl_, mask_, status_, oamAddr_, v_, t_, x_, w_, buffer_, openBus_,
           openBusFrame_, scanline_, dot_, odd_, frames_, dots_, skipDot_, ignoreRead_, preventVbl_, pendingV_, pendingDelay_);
        ar(ntByte_, atByte_, loByte_, hiByte_, bgLo_, bgHi_, atLo_, atHi_);
        ar(copyBuffer_, secAddr_, spriteH_, spriteL_, inRange_, copyDone_, sprite0Added_, sprite0Line_, overflowCount_,
           foundSprites_, lineSprites_, spX_, spAttr_, spLo_, spHi_, spIndex_, secIndex_);
    }

private:
    bool rendering() const { return mask_ & 0x18; }
    uint8_t fetch(uint16_t address);
    uint8_t busRead(uint16_t address) const;
    void busWrite(uint16_t address, uint8_t value);
    void setBusAddress(uint16_t address);
    uint8_t paletteRead(uint16_t address) const;
    void incX();
    void incY();
    void incrementV();
    void backgroundDot();
    void evaluateDot();
    void spriteFetch();
    void extraSprites();
    void pixel();
    void blankPixel();
    void output(int x, int bgIdx, int bgPal, int spIdx, int spPal, int slot);
    uint8_t decayedOpenBus(uint8_t mask);
    void driveOpenBus(uint8_t value, uint8_t mask);

    VideoFrame* frame_ = nullptr;
    Mapper* mapper_ = nullptr;
    bool pal_ = false, drawing_ = true, unlimited_ = false;
    int lastLine_ = 261;  // the pre-render line

    std::array<uint8_t, 0x20> secondary_{};
    uint8_t ctrl_ = 0, mask_ = 0, status_ = 0, oamAddr_ = 0;
    uint16_t v_ = 0, t_ = 0;
    uint8_t x_ = 0;
    bool w_ = false;
    uint8_t buffer_ = 0, openBus_ = 0;
    std::array<uint32_t, 8> openBusFrame_{};  // when each bit of the open bus was last driven
    int16_t scanline_ = 0, dot_ = 0;
    bool odd_ = false, frameDone_ = false, preventVbl_ = false;
    uint32_t frames_ = 0;
    uint64_t dots_ = 0;
    bool skipDot_ = false;
    uint8_t ignoreRead_ = 0;  // dots during which a 2007 read is ignored
    uint16_t pendingV_ = 0;
    uint8_t pendingDelay_ = 0;  // a 2006 write reaches v a few dots later

    // Background fetches and shifters.
    uint8_t ntByte_ = 0, atByte_ = 0, loByte_ = 0, hiByte_ = 0;
    uint16_t bgLo_ = 0, bgHi_ = 0, atLo_ = 0, atHi_ = 0;

    // Sprite evaluation (for the next line) and the sprites of this line.
    uint8_t copyBuffer_ = 0, secAddr_ = 0, spriteH_ = 0, spriteL_ = 0;
    bool inRange_ = false, copyDone_ = false, sprite0Added_ = false, sprite0Line_ = false;
    uint8_t overflowCount_ = 0;
    uint8_t foundSprites_ = 0;  // found by this line's evaluation
    uint8_t lineSprites_ = 0;   // drawn on this line (more than 8 with unlimited sprites)
    std::array<uint8_t, 64> spX_{}, spAttr_{}, spLo_{}, spHi_{};
    std::array<uint8_t, 64> spIndex_{};
    std::array<uint8_t, 8> secIndex_{};  // the OAM number of each sprite in secondary OAM

    std::array<uint32_t, 512> rgb_{};
};

}  // namespace plumnin::nes
