#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../console.h"
#include "nes_apu.h"
#include "nes_cart.h"
#include "nes_ppu.h"

namespace plumnin {

// The NES (and the Famicom): the 2A03 (a 6502 with the sound and the DMAs) and the
// 2C02, NTSC or PAL as the cartridge's header or the chosen region say.
//
// Time is counted in master clock ticks, as the real console divides its crystal: a
// processor cycle is 12 of them on NTSC (16 on PAL), a PPU dot 4 (5). Each memory
// access of the processor is a cycle, and the PPU is run up to the middle of the
// cycle before the access and up to its end after, so that a register read sees the
// PPU at the right dot.
class Nes : public Console {
public:
    Nes();
    ~Nes() override;

    ConsoleSystem system() const override { return ConsoleSystem::Nes; }
    void reset() override;
    void powerCycle() override;
    Region effectiveRegion() const override;
    void runFrame() override;
    const VideoFrame& frame() const override { return *frame_; }
    double refreshRate() const override { return pal_ ? 50.0070 : 60.0988; }
    void setSampleRate(double hz) override {
        outputRate_ = hz;
        apu_.setSampleRate(hz);
    }
    bool synthSupported() const override { return true; }
    void setUnlimitedSprites(bool on) override { ppu_.setUnlimitedSprites(on); }

    bool hasSaveData() const override { return cart_ && cart_->hasBattery(); }
    std::vector<uint8_t> saveData() const override;
    void loadSaveData(const std::vector<uint8_t>& data) override;
    bool saveDataChanged() const override { return cart_ && cart_->prgRamChanged(); }
    void clearSaveDataChanged() override {
        if (cart_) cart_->clearPrgRamChanged();
    }

    CheatDecoder cheatDecoder() const override { return decoder(); }
    void setCheats(const std::vector<CheatPatch>& patches) override;

    std::vector<MemoryRegion> memoryMap() override;
    uint32_t achievementsConsole() const override { return 7; }

    // For tests: memory as the processor sees it (without side effects), the
    // registers, and where the PPU is.
    uint8_t peek(uint16_t address) const;
    uint16_t pc() const { return pc_; }
    int scanline() const { return ppu_.scanline(); }
    int dot() const { return ppu_.dot(); }
    uint64_t cycles() const { return static_cast<uint64_t>(cycle_); }
    const nes::CartInfo* cartInfo() const { return cart_ ? &cart_->info() : nullptr; }
    // The first nametable as text, a line per row: test ROMs print in ASCII tiles.
    std::string screenText() const;

    static CheatDecoder decoder();
    static const char* cheatFormats();

protected:
    void saveBlocks(StateBlocks& blocks) override;
    bool loadBlock(uint32_t id, StateReader& r) override;
    bool insert(const std::vector<uint8_t>& rom, std::string* error) override;
    void regionChanged() override;

public:
    enum class Mode : uint8_t {
        None, Imp, Acc, Imm, Rel, Zero, ZeroX, ZeroY, Ind, IndX, IndY, IndYW, Abs, AbsX, AbsXW, AbsY, AbsYW, Special
    };

private:
    static const Mode kModes[256];
    enum Flag : uint8_t { C = 1, Z = 2, I = 4, D = 8, B = 0x10, U = 0x20, V = 0x40, N = 0x80 };

    void setupRegion();
    void cpuReset(bool soft);

    // One processor cycle around a memory access.
    void startCycle(bool forRead);
    void endCycle(bool forRead);
    void runPpu(uint64_t to) {
        while (ppuClock_ + ppuDivider_ <= to) {
            ppu_.step();
            ppuClock_ += ppuDivider_;
        }
    }
    uint8_t read(uint16_t address) {
        if (needHalt_) dma(address);
        startCycle(true);
        const uint8_t v = busRead(address);
        endCycle(true);
        return v;
    }
    void write(uint16_t address, uint8_t value) {
        startCycle(false);
        busWrite(address, value);
        endCycle(false);
    }
    void dummyRead(uint16_t address) { read(address); }
    uint8_t busRead(uint16_t address);
    void busWrite(uint16_t address, uint8_t value);
    uint8_t dmaRead(uint16_t address, uint16_t& previous, bool internal);
    void dma(uint16_t address);
    void stopDmc();
    uint8_t readPad(int port);
    void latchPads();
    void applyRamCheats();

    // The processor (nes_cpu.cpp).
    void step();
    void setChipVoices(uint32_t full, uint32_t silent, int percent) override { apu_.setChannelGains(full, silent, percent); }
    // Hands the synthesizer what each sound channel is doing, about 480 times a second.
    void pollMusic();
    int64_t lastPoll_ = 0;
    double cpuHz() const { return pal_ ? 1662607.0 : 1789773.0; }
    void execute(uint8_t op);
    void interrupt();
    uint16_t operand(Mode m);
    uint8_t value(Mode m, uint16_t address) {
        return m == Mode::Imm || m == Mode::Rel ? static_cast<uint8_t>(address) : read(address);
    }
    uint8_t fetch() { return read(pc_++); }
    uint16_t fetch16() {
        const uint8_t lo = fetch();
        return static_cast<uint16_t>(lo | fetch() << 8);
    }
    void push(uint8_t v) { write(static_cast<uint16_t>(0x100 | sp_--), v); }
    uint8_t pop() { return read(static_cast<uint16_t>(0x100 | ++sp_)); }
    void setZN(uint8_t v) { p_ = static_cast<uint8_t>((p_ & ~(Z | N)) | (v ? 0 : Z) | (v & N)); }
    void flag(Flag f, bool on) { p_ = static_cast<uint8_t>(on ? p_ | f : p_ & ~f); }
    void branch(uint16_t offset, bool taken);
    void brk();
    // Read-modify-write: the 6502 writes the old value back before the new one.
    template <class F>
    void rmw(uint16_t address, F&& change) {
        const uint8_t v = read(address);
        write(address, v);
        write(address, change(v));
    }
    void adc(uint8_t v);
    void compare(uint8_t reg, uint8_t v);
    uint8_t asl(uint8_t v);
    uint8_t lsr(uint8_t v);
    uint8_t rol(uint8_t v);
    uint8_t ror(uint8_t v);
    void storeHigh(uint8_t index, uint8_t value);

    std::unique_ptr<VideoFrame> frame_;
    std::unique_ptr<nes::Mapper> cart_;
    nes::Ppu ppu_;
    nes::Apu apu_;
    bool pal_ = false;

    // Processor.
    uint8_t a_ = 0, x_ = 0, y_ = 0, sp_ = 0xFD, p_ = I;
    uint16_t pc_ = 0;
    uint16_t base_ = 0;  // the address before indexing, for the SHx instructions
    bool jammed_ = false;
    // Interrupt lines, sampled at the end of each cycle as the 6502 does.
    bool needNmi_ = false, prevNeedNmi_ = false, prevNmiLine_ = false;
    bool runIrq_ = false, prevRunIrq_ = false;
    // Clock.
    int64_t cycle_ = 0;
    uint64_t master_ = 0, ppuClock_ = 0;
    uint8_t startClocks_ = 6, endClocks_ = 6, ppuDivider_ = 4;
    // DMA: the sprite DMA (4014) and the DMC's sample fetches.
    bool needHalt_ = false, needDummyRead_ = false, dmcRunning_ = false, abortDmc_ = false, spriteDma_ = false;
    uint8_t spriteDmaPage_ = 0;

    std::array<uint8_t, 0x800> ram_{};
    uint8_t openBus_ = 0;
    bool strobe_ = false;
    std::array<uint8_t, 2> shift_{};
    std::array<uint8_t, 2> padBits_{};  // the buttons of each pad, in the order the NES reads them

    std::vector<CheatPatch> romCheats_, ramCheats_;
};

}  // namespace plumnin
