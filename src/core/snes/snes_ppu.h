#pragma once

#include <array>
#include <cstdint>

#include "../state_io.h"
#include "../video_frame.h"

namespace plumnin::snes {

// The picture processor (the S-PPU1 and S-PPU2), a line at a time: the eight modes
// with their four backgrounds (mode 7's rotation, offset per tile, the high
// resolution of modes 5 and 6), the 128 sprites with their limits of 32 a line and 34
// tiles, the two windows, mosaic, colour math with the sub screen and the brightness.
//
// Each line is drawn when the console reaches it, with the registers as they are
// then: what HDMA changes between lines shows on the next one, as on the real console.
class Ppu {
public:
    Ppu();
    void setFrame(VideoFrame* f) { frame_ = f; }
    void power();
    void reset();
    void setUnlimitedSprites(bool on) { unlimited_ = on; }

    // The registers, from the B bus (21xx). A read gives the PPU's open bus where the
    // register has bits it does not drive.
    uint8_t read(uint8_t reg, uint8_t mdr);
    void write(uint8_t reg, uint8_t value);
    uint8_t peekVram(uint32_t byteAddress) const {
        const uint16_t w = vram_[(byteAddress >> 1) & 0x7FFF];
        return static_cast<uint8_t>(byteAddress & 1 ? w >> 8 : w);
    }

    // The console tells where the beam is: the line, the dot (for the counters latched
    // by 2137 or by the I/O port) and whether the picture is in vblank.
    void setBeam(int line, int dot) {
        line_ = line;
        dot_ = dot;
    }
    void latchCounters();
    // A new frame begins (line 0), the visible lines, and the start of vblank.
    void startFrame(bool pal);
    void drawLine(int line, bool drawing);
    void startVblank();
    bool overscan() const { return setini_ & 0x04; }
    bool interlace() const { return setini_ & 0x01; }
    // The lines of picture: 224, or 239 with the overscan bit.
    int visibleLines() const { return overscan() ? 239 : 224; }
    bool forceBlank() const { return inidisp_ & 0x80; }
    // Where a background's tile map starts in VRAM (words), for tests that read the screen.
    uint16_t mapBase(int bg) const { return static_cast<uint16_t>((bgsc_[static_cast<size_t>(bg & 3)] & 0xFC) << 8); }
    uint16_t vramWord(uint16_t address) const { return vram_[address & 0x7FFF]; }

    template <class Ar>
    void state(Ar& ar) {
        ar(vram_, cgram_, oam_, highOam_);
        ar(inidisp_, obsel_, oamAddr_, oamBase_, oamPriority_, oamLatch_, bgmode_, mosaic_, bgsc_, bgnba_, hofs_, vofs_,
           ofsLatch_, m7Latch_, vmain_, vmaddr_, vramLatch_, m7sel_, m7_, m7x_, m7y_, m7hofs_, m7vofs_, cgaddr_,
           cgHigh_, cgLatch_, w12sel_, wobjsel_, wh_, wbglog_, wobjlog_, tm_, ts_, tmw_, tsw_, cgwsel_, cgadsub_, fixed_,
           setini_, hcounter_, vcounter_, hFlip_, vFlip_, latched_, timeOver_, rangeOver_, field_, ppu1Bus_, ppu2Bus_,
           mosaicLine_, pal_, line_, dot_, hires_);
    }

private:
    struct LayerLine {
        // For each column: the colour (BGR555), the CGRAM index + 1 (0: transparent) and
        // the priority bit, or for sprites their priority 0-3.
        std::array<uint16_t, 512> color{};
        std::array<uint16_t, 512> index{};
        std::array<uint8_t, 512> priority{};
        std::array<uint16_t, 512> source{};
        std::array<bool, 512> math{};  // sprites: palettes 4-7 take part in colour math
    };

    uint16_t vramAddress() const;
    void vramPrefetch();
    void bumpVram(bool high);
    void writeOam(uint8_t v);
    uint8_t readOam();

    void drawBackground(int bg, int y, int width, LayerLine& out);
    void drawMode7(int y, LayerLine& bg1, LayerLine& bg2, bool extbg);
    void drawSprites(int y, LayerLine& out);
    void windowMask(int layer, std::array<bool, 256>& mask) const;
    uint16_t paletteColor(int index) const { return cgram_[index & 0xFF] & 0x7FFF; }
    void output(int row, int width, bool hires);
    void expandToHires();

    VideoFrame* frame_ = nullptr;
    bool unlimited_ = false;

    std::array<uint16_t, 0x8000> vram_{};
    std::array<uint16_t, 256> cgram_{};
    std::array<uint8_t, 512> oam_{};
    std::array<uint8_t, 32> highOam_{};

    uint8_t inidisp_ = 0x80, obsel_ = 0;
    uint16_t oamAddr_ = 0, oamBase_ = 0;  // byte address in OAM (0-543), and the word address written
    bool oamPriority_ = false;
    uint8_t oamLatch_ = 0;
    uint8_t bgmode_ = 0, mosaic_ = 0;
    std::array<uint8_t, 4> bgsc_{}, bgnba_{};
    std::array<uint16_t, 4> hofs_{}, vofs_{};
    uint8_t ofsLatch_ = 0, m7Latch_ = 0;
    uint8_t vmain_ = 0;
    uint16_t vmaddr_ = 0, vramLatch_ = 0;
    uint8_t m7sel_ = 0;
    std::array<int16_t, 4> m7_{};  // A, B, C, D
    int16_t m7x_ = 0, m7y_ = 0, m7hofs_ = 0, m7vofs_ = 0;
    uint8_t cgaddr_ = 0;
    bool cgHigh_ = false;
    uint8_t cgLatch_ = 0;
    std::array<uint8_t, 2> w12sel_{};  // 2123 (BG1 BG2), 2124 (BG3 BG4)
    uint8_t wobjsel_ = 0;              // 2125 (OBJ, colour)
    std::array<uint8_t, 4> wh_{};      // left and right of the two windows
    uint8_t wbglog_ = 0, wobjlog_ = 0, tm_ = 0, ts_ = 0, tmw_ = 0, tsw_ = 0, cgwsel_ = 0, cgadsub_ = 0;
    std::array<uint8_t, 3> fixed_{};  // the fixed colour's red, green and blue
    uint8_t setini_ = 0;
    uint16_t hcounter_ = 0, vcounter_ = 0;
    bool hFlip_ = false, vFlip_ = false, latched_ = false;
    bool timeOver_ = false, rangeOver_ = false, field_ = false;
    uint8_t ppu1Bus_ = 0, ppu2Bus_ = 0;
    int16_t mosaicLine_ = 1;
    bool pal_ = false;
    int16_t line_ = 0, dot_ = 0;
    bool hires_ = false;  // this frame has lines of 512 columns

    // The lines being put together (not part of the state: drawn anew every line).
    std::array<LayerLine, 4> bg_;
    LayerLine obj_;
    std::array<uint32_t, 32768> rgb_{};
};

}  // namespace plumnin::snes
