#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../console.h"
#include "snes_apu.h"
#include "snes_cart.h"
#include "snes_cpu.h"
#include "snes_ppu.h"

namespace plumnin {

// The Super Nintendo (and the Super Famicom): the 5A22 (a 65C816 with the DMA, the
// multiplier and the timers), the two PPUs and the sound module, NTSC or PAL.
//
// Time is counted in master clock ticks (21.48 MHz on NTSC): a line is 1364 of them,
// a memory access 6, 8 or 12 depending on where it goes, and the sound module runs
// behind on its own clock and catches up when the processor talks to it.
class Snes : public Console, private snes::CpuBus {
public:
    Snes();
    ~Snes() override;

    ConsoleSystem system() const override { return ConsoleSystem::Snes; }
    std::string title() const override { return cart_.info().title; }
    void reset() override;
    void powerCycle() override;
    Region effectiveRegion() const override;
    void runFrame() override;
    const VideoFrame& frame() const override { return *frame_; }
    double refreshRate() const override { return pal_ ? 50.0070 : 60.0988; }
    void setSampleRate(double hz) override { apu_.setSampleRate(hz); }
    void setUnlimitedSprites(bool on) override { ppu_.setUnlimitedSprites(on); }

    bool hasSaveData() const override { return cart_.info().battery && !cart_.ram().empty(); }
    std::vector<uint8_t> saveData() const override;
    void loadSaveData(const std::vector<uint8_t>& data) override;
    bool saveDataChanged() const override { return cart_.ramChanged(); }
    void clearSaveDataChanged() override { cart_.clearRamChanged(); }

    CheatDecoder cheatDecoder() const override { return decoder(); }
    void setCheats(const std::vector<CheatPatch>& patches) override;

    std::vector<MemoryRegion> memoryMap() override;
    uint32_t achievementsConsole() const override { return 3; }

    // For tests: memory as the processor sees it (without side effects), the
    // processor's registers, where the beam is, and the VRAM.
    uint8_t peek(uint32_t address);
    const snes::Cpu::Regs& cpuRegs() const { return cpu_.r; }
    int scanline() const { return line_; }
    uint64_t masterClock() const { return master_; }
    const snes::CartInfo& cartInfo() const { return cart_.info(); }
    uint8_t vramByte(uint32_t address) const { return ppu_.peekVram(address); }
    std::array<uint8_t, 0x10000>& apuRam() { return apu_.ram(); }
    // The first background's map as text, a line per row, its tiles taken as ASCII
    // from `first` on: test ROMs print in ASCII tiles.
    std::string screenText(int first = 0x20) const;

    static CheatDecoder decoder();
    static const char* cheatFormats();

protected:
    void saveBlocks(StateBlocks& blocks) override;
    bool loadBlock(uint32_t id, StateReader& r) override;
    bool insert(const std::vector<uint8_t>& rom, std::string* error) override;
    void regionChanged() override;

private:
    struct Channel {
        uint8_t dmap = 0xFF, bbad = 0xFF, a1b = 0xFF, dasb = 0xFF, ntrl = 0xFF, unused = 0xFF;
        uint16_t a1t = 0xFFFF, das = 0xFFFF, a2a = 0xFFFF;
        bool hdmaDone = false, hdmaDo = false;
    };

    // The bus as the processor drives it (snes::CpuBus).
    uint8_t read(uint32_t address) override;
    void write(uint32_t address, uint8_t value) override;
    void idle() override { advance(6); }

    void setupRegion();
    int speed(uint32_t address) const;
    void advance(int clocks);
    int nextEvent() const;
    void event(int at);
    void startLine();
    uint8_t busRead(uint32_t address);
    void busWrite(uint32_t address, uint8_t value);
    uint8_t bRead(uint8_t reg);
    void bWrite(uint8_t reg, uint8_t value);
    uint8_t ioRead(uint16_t address);
    void ioWrite(uint16_t address, uint8_t value);
    uint8_t dmaRegRead(uint16_t address) const;
    void dmaRegWrite(uint16_t address, uint8_t value);
    uint8_t dmaRead(uint32_t address);
    void runDma(uint8_t channels);
    void hdmaSetup();
    void hdmaRun();
    void hdmaReload(Channel& c, int& cost);
    void updateIrq();
    uint16_t padBits(int port) const;
    void autoJoypad();
    void applyRamCheats();

    std::unique_ptr<VideoFrame> frame_;
    snes::Cart cart_;
    snes::Cpu cpu_;
    snes::Ppu ppu_;
    snes::Apu apu_;
    bool pal_ = false;
    bool loaded_ = false;

    // Clock and beam.
    uint64_t master_ = 0;
    int hc_ = 0;  // master clocks into the line
    int line_ = 0;
    bool frameDone_ = false;
    bool refreshed_ = false, hdmaDoneLine_ = false, irqDoneLine_ = false;

    std::array<uint8_t, 0x20000> wram_{};
    uint32_t wmadd_ = 0;
    uint8_t mdr_ = 0;  // the open bus: the last value on the data bus

    // The 5A22's registers.
    uint8_t nmitimen_ = 0, wrio_ = 0xFF, wrmpya_ = 0xFF, wrmpyb_ = 0, memsel_ = 0, hdmaen_ = 0;
    uint16_t wrdiv_ = 0xFFFF, rddiv_ = 0, rdmpy_ = 0, htime_ = 0x1FF, vtime_ = 0x1FF;
    bool rdnmi_ = false, timeup_ = false;
    int autoJoyBusyUntil_ = -1;
    std::array<uint16_t, 4> joy_{};
    bool joyStrobe_ = false;
    std::array<uint16_t, 2> joyShift_{};
    std::array<Channel, 8> dma_{};

    std::vector<CheatPatch> romCheats_, ramCheats_;
};

}  // namespace plumnin
