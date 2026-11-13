#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../console.h"
#include "gb_apu.h"
#include "gb_cart.h"
#include "gb_ppu.h"

namespace plumnin {

// The Game Boy and the Game Boy Color. Which one is decided by the cartridge: a game
// made for the Color (bit 7 of byte 0x143) runs on a Color, the rest on a Game Boy,
// with its four shades. It starts where the boot ROM leaves the console.
class GameBoy : public Console {
public:
    GameBoy();
    ~GameBoy() override;

    ConsoleSystem system() const override { return color_ ? ConsoleSystem::GameBoyColor : ConsoleSystem::GameBoy; }
    std::string title() const override { return cart_.title(); }
    void reset() override;
    void powerCycle() override;
    int ports() const override { return 1; }
    void runFrame() override;
    const VideoFrame& frame() const override { return *frame_; }
    double refreshRate() const override { return 4194304.0 / gb::kDotsPerFrame; }
    void setSampleRate(double hz) override {
        outputRate_ = hz;
        apu_.setSampleRate(hz);
    }
    bool synthSupported() const override { return true; }

    bool hasSaveData() const override { return cart_.hasBattery(); }
    std::vector<uint8_t> saveData() const override { return cart_.saveData(); }
    void loadSaveData(const std::vector<uint8_t>& data) override { cart_.loadSaveData(data); }
    bool saveDataChanged() const override { return cart_.ramChanged(); }
    void clearSaveDataChanged() override { cart_.clearRamChanged(); }

    CheatDecoder cheatDecoder() const override;
    void setCheats(const std::vector<CheatPatch>& patches) override;

    std::vector<MemoryRegion> memoryMap() override;
    uint32_t achievementsConsole() const override { return color_ ? 6 : 4; }

    // The shades of a Game Boy game, lightest first (the green of its screen by default).
    void setDmgPalette(const std::array<uint32_t, 4>& colours) { ppu_.setDmgPalette(colours); }
    // What the game sent through the link cable (blargg's tests print their results there).
    const std::string& serialOutput() const { return serial_; }
    // The registers, for tests: A F B C D E H L, then SP and PC.
    std::array<uint16_t, 4> registers() const {
        return {static_cast<uint16_t>(a_ << 8 | f_), static_cast<uint16_t>(b_ << 8 | c_),
                static_cast<uint16_t>(d_ << 8 | e_), static_cast<uint16_t>(h_ << 8 | l_)};
    }
    // The last instruction run was LD B,B, which test ROMs use to say they are done.
    bool hitBreakpoint() const { return breakpoint_; }
    uint8_t peek(uint16_t address) const;

    static CheatDecoder decoder();
    static const char* cheatFormats();

protected:
    void saveBlocks(StateBlocks& blocks) override;
    bool loadBlock(uint32_t id, StateReader& r) override;
    bool insert(const std::vector<uint8_t>& rom, std::string* error) override;

private:
    // One M-cycle of everything but the processor: timer, picture, sound, DMA, link.
    void tick();
    uint8_t read(uint16_t address) {
        tick();
        return busRead(address);
    }
    void write(uint16_t address, uint8_t value) {
        tick();
        busWrite(address, value);
    }
    uint8_t busRead(uint16_t address) const;
    uint8_t dmaRead(uint16_t address) const;
    void busWrite(uint16_t address, uint8_t value);
    uint8_t readIo(uint16_t address) const;
    void writeIo(uint16_t address, uint8_t value);
    void setDiv(uint16_t value);
    void timaIncrement();
    bool timerBit(uint16_t div) const;
    void hdmaBlock();
    uint8_t joypad() const;
    void updateJoypad();
    void applyRamCheats();

    // The processor (gb_cpu.cpp).
    void step();
    void execute(uint8_t op);
    void executeCb();
    void interrupt();
    uint8_t fetch() {
        const uint8_t v = read(pc_);
        if (haltBug_) haltBug_ = false;
        else ++pc_;
        return v;
    }
    uint16_t fetch16() {
        const uint8_t lo = fetch();
        return static_cast<uint16_t>(lo | fetch() << 8);
    }
    void push(uint16_t v);
    uint16_t pop();
    uint8_t& reg(int r);
    uint8_t getR(int r);
    void setR(int r, uint8_t v);
    void alu(int op, uint8_t v);
    uint8_t inc(uint8_t v);
    uint8_t dec(uint8_t v);
    uint8_t cb(int op, uint8_t v);
    bool condition(int cc) const;
    void setChipVoices(uint32_t full, int percent) override { apu_.setChannelGains(full, percent); }
    // Hands the synthesizer what each sound channel is doing.
    void pollMusic();

    std::unique_ptr<VideoFrame> frame_;
    gb::Cartridge cart_;
    gb::Ppu ppu_;
    gb::Apu apu_;
    bool color_ = false;

    // Processor.
    uint8_t a_ = 0, f_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0, h_ = 0, l_ = 0;
    uint16_t sp_ = 0, pc_ = 0;
    bool ime_ = false, eiDelay_ = false, halted_ = false, haltBug_ = false, stopped_ = false;
    bool breakpoint_ = false;

    // Memory and registers.
    std::array<uint8_t, 0x8000> wram_{};
    std::array<uint8_t, 0x7F> hram_{};
    uint8_t wramBank_ = 1;
    uint8_t if_ = 0xE1, ie_ = 0;
    uint8_t p1_ = 0x30, sb_ = 0, sc_ = 0;
    uint8_t buttons_ = 0x00;  // pressed, 1 = held: right left up down a b select start
    uint16_t div_ = 0;
    uint8_t tima_ = 0, tma_ = 0, tac_ = 0;
    bool timaOverflow_ = false, timaReloaded_ = false, timaIrqNew_ = false;
    int serialCycles_ = 0;  // bits left to shift
    bool doubleSpeed_ = false;
    uint8_t key1_ = 0;
    // OAM DMA.
    bool dmaActive_ = false, dmaCopying_ = false;
    uint8_t dmaDelay_ = 0, dmaSource_ = 0, dmaNextSource_ = 0, dmaIndex_ = 0, dmaReg_ = 0xFF;
    // The Color's VRAM DMA.
    uint16_t hdmaSource_ = 0, hdmaDest_ = 0;
    uint8_t hdmaLength_ = 0x7F;
    bool hdmaActive_ = false;

    uint64_t cycles_ = 0;  // M-cycles since power on
    std::string serial_;
    std::vector<CheatPatch> romCheats_, ramCheats_;
};

}  // namespace plumnin
