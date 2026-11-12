#pragma once

#include <array>
#include <cstdint>

#include "../state_io.h"
#include "../video_frame.h"

namespace plumnin::gb {

constexpr int kScreenWidth = 160, kScreenHeight = 144;
constexpr int kDotsPerLine = 456, kLines = 154;
constexpr int kDotsPerFrame = kDotsPerLine * kLines;  // 70224

// The picture processor: modes and their interrupts dot by dot, and each line drawn
// when its mode 3 ends, with the background, the window and the sprites kept apart
// for the 3D view.
class Ppu {
public:
    enum Interrupt : uint8_t { VBlank = 1, Stat = 2 };

    void reset(bool color);
    // `dots` of the 4194304 Hz clock; the interrupts it raises are or'ed into `irq`.
    void run(int dots, uint8_t& irq);
    uint8_t read(uint16_t address) const;
    void write(uint16_t address, uint8_t value, uint8_t& irq);

    void setFrame(VideoFrame* frame) { frame_ = frame; }
    void setDrawing(bool on) { drawing_ = on; }
    // The four shades of a Game Boy game, 0xFFRRGGBB, lightest first.
    void setDmgPalette(const std::array<uint32_t, 4>& colours) { dmg_ = colours; }

    // A frame ended (the vertical blank started, or 70224 dots went by with the LCD off).
    bool takeFrameDone() {
        const bool d = frameDone_;
        frameDone_ = false;
        return d;
    }
    // A horizontal blank started: the Color's HBlank DMA copies 16 bytes.
    bool takeHblank() {
        const bool h = hblank_;
        hblank_ = false;
        return h;
    }
    bool lcdOn() const { return lcdc_ & 0x80; }
    bool color() const { return color_; }

    uint8_t vram[2][0x2000] = {};
    uint8_t oam[0xA0] = {};
    uint8_t vramBank = 0;

    template <class Ar>
    void state(Ar& ar) {
        ar(vram[0], vram[1], oam, vramBank, lcdc_, stat_, scy_, scx_, ly_, lyc_, bgp_, obp_, wy_, wx_, line_, dot_,
           mode_, mode3End_, statLine_, wyHit_, winLine_, firstLine_, offDots_, frameDone_, hblank_, bcps_, ocps_,
           bgPal_, obPal_, opri_, unwrappedScx_, sprites_, spriteCount_);
    }

private:
    void setMode(int mode, uint8_t& irq);
    void updateStat(uint8_t& irq);
    int mode3Length();
    void renderLine();
    void blankFrame();
    uint32_t cgbColour(const uint8_t* pal, int palette, int index) const;

    bool color_ = false;
    VideoFrame* frame_ = nullptr;
    bool drawing_ = true;
    std::array<uint32_t, 4> dmg_{0xFFE0F8D0u, 0xFF88C070u, 0xFF346856u, 0xFF081820u};

    uint8_t lcdc_ = 0x91, stat_ = 0, scy_ = 0, scx_ = 0, ly_ = 0, lyc_ = 0, bgp_ = 0xFC;
    uint8_t obp_[2] = {0xFF, 0xFF};
    uint8_t wy_ = 0, wx_ = 0;
    int line_ = 0, dot_ = 0, mode_ = 0, mode3End_ = 252;
    bool statLine_ = false;
    bool wyHit_ = false;
    int winLine_ = 0;
    bool firstLine_ = false;  // the line after the LCD is switched on has no mode 2
    int offDots_ = 0;
    bool frameDone_ = false, hblank_ = false;
    uint8_t bcps_ = 0, ocps_ = 0;
    uint8_t bgPal_[64] = {}, obPal_[64] = {};
    uint8_t opri_ = 0;
    int32_t unwrappedScx_ = 0;
    uint8_t sprites_[10] = {};  // the OAM entries on this line, in drawing priority
    uint8_t spriteCount_ = 0;  // SCX that keeps counting past 255, for the 3D camera
};

}  // namespace plumnin::gb
