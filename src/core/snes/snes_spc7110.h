#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// The SPC7110 (Far East of Eden Zero, Momotarou Dentetsu Happy, Super Power League 4):
// Epson's memory controller for ROMs of up to 7 MB. The first megabyte is the program;
// the rest, the data ROM, is reached through 1 MB banks at D0-FF, a data port that
// reads it at an offset with strides and adjustments (4810-481A), and a decompressor
// of 1, 2 and 4 bpp graphics that hands out finished tiles (4800). It also has a
// multiplier and a divider (4820-482F) and gates the save RAM at 6000-7FFF.
//
// Far East of Eden Zero's clock (an Epson RTC-4513 at 4840) is not emulated.
class Spc7110 : public Coprocessor {
public:
    Spc7110(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram) : rom_(rom), ram_(ram) {}

    void power() override;
    void reset() override { power(); }
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t) override {}
    void setMasterRate(double) override {}
    void catchUp() override {}
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }

private:
    static constexpr uint32_t kProgramSize = 0x100000;

    uint8_t ioRead(uint8_t reg);
    void ioWrite(uint8_t reg, uint8_t value);
    uint8_t mcuRomRead(uint32_t address) const;
    uint8_t programRead(uint32_t offset) const;
    uint8_t dataRead(uint32_t address) const;

    // The decompression unit.
    void dcuLoadAddress();
    void dcuBeginTransfer();
    uint8_t dcuRead();

    // The data port.
    uint32_t dataOffset() const { return r_[0x11] | r_[0x12] << 8 | static_cast<uint32_t>(r_[0x13]) << 16; }
    uint16_t dataAdjust() const { return static_cast<uint16_t>(r_[0x14] | r_[0x15] << 8); }
    uint16_t dataStride() const { return static_cast<uint16_t>(r_[0x16] | r_[0x17] << 8); }
    void setDataOffset(uint32_t v);
    void setDataAdjust(uint32_t v);
    void dataPortRead();
    void dataPortIncrement();  // after a read of 4810
    void dataPortSeek(int mode);  // 4814, 4815 or 481A, by 4818's top bits

    void aluMultiply();
    void aluDivide();

    // The decompressor (neviksti's analysis, as ares writes it): an arithmetic decoder
    // with a context model by bitplane, and a most-recently-used colour list.
    void decompInit(uint32_t mode, uint32_t origin);
    void decompDecode();
    uint8_t decompReadByte() { return dataRead(dOffset_++); }

    template <class Ar>
    void state(Ar& ar) {
        ar(r_, dcuMode_, dcuAddress_, dcuOffset_, dcuTile_);
        ar(ctxPrediction_, ctxSwap_, dBpp_, dOffset_, dBits_, dRange_, dInput_, dOutput_, dPixels_, dColormap_,
           dResult_);
    }

    const std::vector<uint8_t>& rom_;
    std::vector<uint8_t>& ram_;
    std::array<uint8_t, 0x40> r_{};  // 4800-483F
    uint8_t dcuMode_ = 0;
    uint32_t dcuAddress_ = 0, dcuOffset_ = 0;
    std::array<uint8_t, 32> dcuTile_{};

    std::array<uint8_t, 75> ctxPrediction_{}, ctxSwap_{};  // 5 sets of 15
    uint32_t dBpp_ = 1, dOffset_ = 0, dBits_ = 8;
    uint16_t dRange_ = 0, dInput_ = 0;
    uint8_t dOutput_ = 0;
    uint64_t dPixels_ = 0, dColormap_ = 0;
    uint32_t dResult_ = 0;
};

}  // namespace plumnin::snes
