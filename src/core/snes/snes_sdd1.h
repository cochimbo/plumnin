#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "snes_copro.h"

namespace plumnin::snes {

// The S-DD1 (Star Ocean, Street Fighter Alpha 2): it maps up to 8 MB of ROM in 1 MB
// banks at C0-FF, and decompresses graphics on the fly while a DMA reads them. The game
// arms a channel at 4800/4801 and starts a DMA from a fixed address in C0-FF; each byte
// the channel reads from there then comes out of the decompressor instead of the ROM,
// for as many bytes as the DMA moves. To know those, the chip watches the writes to the
// DMA registers (43x2-43x6), as the real one does on the bus.
class Sdd1 : public Coprocessor {
public:
    Sdd1(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram) : rom_(rom), ram_(ram) {}

    void power() override;
    void reset() override {}
    bool read(uint32_t address, uint8_t& value) override;
    bool write(uint32_t address, uint8_t value) override;
    void addTime(uint32_t) override {}
    void setMasterRate(double) override {}
    void catchUp() override {}
    void save(StateWriter& w) override { state(w); }
    void load(StateReader& r) override { state(r); }

private:
    uint8_t romAt(uint32_t offset) const { return rom_.empty() ? 0 : rom_[offset % rom_.size()]; }
    uint8_t mmcRead(uint32_t address) const;  // C0-FF, through the four bank registers

    // The decompressor (after the analysis by Andreas Naive): an adaptive binary coder
    // with eight Golomb-code bit generators, 32 contexts, and the bitplane order of the
    // header's two top bits.
    void decompressInit(uint32_t address);
    uint8_t decompress();
    uint8_t codeWord(uint8_t length);
    uint8_t bgBit(int code, bool& endOfRun);
    uint8_t pemBit(uint8_t context);
    uint8_t cmBit();

    template <class Ar>
    void state(Ar& ar) {
        ar(r4800_, r4801_, bank_, dmaAddress_, dmaSize_, dmaReady_);
        ar(imOffset_, imBits_, bgMps_, bgLps_, ctxStatus_, ctxMps_, bitplanes_, contextBits_, bitNumber_, bitplane_,
           previous_, r0_, r1_, r2_);
    }

    const std::vector<uint8_t>& rom_;
    std::vector<uint8_t>& ram_;  // also at 00-3F:6000-7FFF on the boards that have it
    uint8_t r4800_ = 0, r4801_ = 0;
    std::array<uint8_t, 4> bank_{0, 1, 2, 3};
    std::array<uint32_t, 8> dmaAddress_{};
    std::array<uint16_t, 8> dmaSize_{};
    bool dmaReady_ = false;

    uint32_t imOffset_ = 0;
    uint8_t imBits_ = 0;
    std::array<uint8_t, 8> bgMps_{};
    std::array<bool, 8> bgLps_{};
    std::array<uint8_t, 32> ctxStatus_{}, ctxMps_{};
    uint8_t bitplanes_ = 0, contextBits_ = 0, bitNumber_ = 0, bitplane_ = 0;
    std::array<uint16_t, 8> previous_{};
    uint8_t r0_ = 0, r1_ = 0, r2_ = 0;
};

}  // namespace plumnin::snes
