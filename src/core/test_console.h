#pragma once

#include <array>
#include <memory>

#include "console.h"

namespace plumnin {

// Not a real console: a test pattern that behaves like one, so everything around the
// consoles (the frontend, the 3D view, savestates, rewind, run-ahead, netplay, the
// headless runner) can be tried and tested before a console is emulated, and without
// any commercial ROM. It takes any file as its cartridge.
//
// It draws a backdrop that changes colour down the screen, a checkerboard on the back
// plane that scrolls sideways, a few solid platforms on the front plane and a 16x16
// sprite that the pads' crosses move (the first pad's A and B change its colour, Start puts it
// back in the middle). A held A also plays a tone. Its "RAM" holds the sprite's place
// and a frame counter, which is what the achievements and netplay look at.
class TestConsole : public Console {
public:
    // It looks like `system`: the Game Boy's 160x144 screen, a TV's 256x224 for the rest.
    explicit TestConsole(ConsoleSystem system = ConsoleSystem::Nes);

    ConsoleSystem system() const override { return system_; }
    std::string title() const override { return "Test pattern"; }
    void reset() override;
    void powerCycle() override;
    int ports() const override { return system_ == ConsoleSystem::GameBoy || system_ == ConsoleSystem::GameBoyColor ? 1 : 2; }
    void runFrame() override;
    const VideoFrame& frame() const override { return *frame_; }
    double refreshRate() const override { return 60.0; }
    void setSampleRate(double hz) override { sampleRate_ = hz; }
    void setWidescreen(int left, int right) override;
    std::vector<MemoryRegion> memoryMap() override { return {{0, ram_.data(), ram_.size()}}; }

    // Where the sprite is, for tests.
    int spriteX() const { return ram_[0] | ram_[1] << 8; }
    int spriteY() const { return ram_[2]; }

protected:
    void saveBlocks(StateBlocks& blocks) override;
    bool loadBlock(uint32_t id, StateReader& r) override;
    bool insert(const std::vector<uint8_t>& rom, std::string* error) override;

private:
    void draw();

    ConsoleSystem system_;
    std::unique_ptr<VideoFrame> frame_;
    std::array<uint8_t, 256> ram_{};  // 0-1 sprite x, 2 sprite y, 3 colour, 4-7 frame counter
    double sampleRate_ = 44100;
    double phase_ = 0;
    int wideLeft_ = 0, wideRight_ = 0;
};

}  // namespace plumnin
