#include "test_console.h"

#include <algorithm>
#include <cmath>

namespace plumnin {

namespace {

constexpr int kSprite = 16;
const uint32_t kSpriteColours[4] = {0xFFF8D030, 0xFF40C8F8, 0xFFF85898, 0xFF78F878};

bool handheld(ConsoleSystem s) { return s == ConsoleSystem::GameBoy || s == ConsoleSystem::GameBoyColor; }

}  // namespace

TestConsole::TestConsole(ConsoleSystem system) : system_(system), frame_(std::make_unique<VideoFrame>()) {
    powerCycle();
    draw();
}

void TestConsole::reset() {
    const int w = handheld(system_) ? 160 : 256, h = handheld(system_) ? 144 : 224;
    const int x = (w - kSprite) / 2, y = (h - kSprite) / 2;
    ram_[0] = static_cast<uint8_t>(x);
    ram_[1] = static_cast<uint8_t>(x >> 8);
    ram_[2] = static_cast<uint8_t>(y);
    ram_[3] = 0;
}

void TestConsole::powerCycle() {
    ram_.fill(0);
    phase_ = 0;
    reset();
}

bool TestConsole::insert(const std::vector<uint8_t>&, std::string*) {
    powerCycle();
    return true;
}

void TestConsole::setWidescreen(int left, int right) {
    wideLeft_ = handheld(system_) ? 0 : std::clamp(left, 0, VideoFrame::kMaxWideSide);
    wideRight_ = handheld(system_) ? 0 : std::clamp(right, 0, VideoFrame::kMaxWideSide);
}

void TestConsole::runFrame() {
    const PadState& p = pads_[0];
    // The second player's cross moves the sprite too, so that both pads change the game.
    const PadState none{};
    const PadState& q = ports() > 1 && padTypes_[1] != PadType::None ? pads_[1] : none;
    const int w = handheld(system_) ? 160 : 256, h = handheld(system_) ? 144 : 224;
    int x = spriteX(), y = spriteY();
    if (padTypes_[0] != PadType::None) {
        if (p.left) --x;
        if (p.right) ++x;
        if (p.up) --y;
        if (p.down) ++y;
        if (p.a) ram_[3] = 1;
        if (p.b) ram_[3] = 2;
        if (q.left) --x;
        if (q.right) ++x;
        if (q.up) --y;
        if (q.down) ++y;
        if (p.start) {
            reset();
            x = spriteX();
            y = spriteY();
        }
    }
    x = std::clamp(x, 0, w - kSprite);
    y = std::clamp(y, 0, h - kSprite);
    ram_[0] = static_cast<uint8_t>(x);
    ram_[1] = static_cast<uint8_t>(x >> 8);
    ram_[2] = static_cast<uint8_t>(y);
    ++frames_;
    const uint32_t count = static_cast<uint32_t>(frames_);
    for (int i = 0; i < 4; ++i) ram_[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(count >> (8 * i));

    // A frame's worth of sound: a 440 Hz square wave while A is held.
    const int samples = static_cast<int>(std::lround(sampleRate_ / refreshRate()));
    std::vector<int16_t>& out = audio_.output();
    for (int i = 0; i < samples; ++i) {
        int16_t v = 0;
        if (p.a && padTypes_[0] != PadType::None) {
            v = phase_ < 0.5 ? 3000 : -3000;
            phase_ += 440.0 / sampleRate_;
            if (phase_ >= 1) phase_ -= 1;
        }
        out.push_back(v);
        out.push_back(v);
    }
    if (drawing_) draw();
}

void TestConsole::draw() {
    VideoFrame& f = *frame_;
    const bool hand = handheld(system_);
    const int cw = hand ? 160 : 256;
    f.height = hand ? 144 : 224;
    f.wideLeft = wideLeft_;
    f.wideRight = wideRight_;
    f.width = cw + wideLeft_ + wideRight_;
    f.interlaced = false;
    f.aspectLines = hand ? 144 : 224;
    f.screenAspect = hand ? 160.0f / 144.0f : 0;
    f.frameNumber = frames_;
    const int scroll = static_cast<int>(frames_ % 32);
    const int sx = spriteX() + wideLeft_, sy = spriteY();
    for (int y = 0; y < f.height; ++y) {
        const uint32_t backdrop = 0xFF000000u | static_cast<uint32_t>(0x20 + y * 0x60 / f.height) << 8 |
                                  static_cast<uint32_t>(0x60 + y * 0x80 / f.height);
        f.backdrop[static_cast<size_t>(y)] = backdrop;
        f.lines[static_cast<size_t>(y)] = {0, static_cast<int16_t>(scroll), true};
        for (int x = 0; x < f.width; ++x) {
            const size_t i = static_cast<size_t>(y) * VideoFrame::kMaxWidth + static_cast<size_t>(x);
            // Back plane: a checkerboard of 16-pixel squares, every other one empty.
            const bool check = (((x + scroll) / 16) + (y / 16)) % 2 == 0;
            f.planeB[i] = check ? 0xFF3050A0u : 0;
            f.planeBInfo[i] = {static_cast<uint8_t>(check ? 1 : 0), Layer::Back, false, Shade::Normal,
                               static_cast<uint16_t>((x + scroll) / 8 + y / 8 * 64)};
            // Front plane: platforms of solid 8x8 tiles on a few rows.
            const int row = y / 8;
            const bool platform = (row % 6 == 4) && ((x / 8) % 10 < 6);
            f.planeA[i] = platform ? ((x + y) % 8 == 0 ? 0xFF206020u : 0xFF40A040u) : 0;
            f.planeAInfo[i] = {static_cast<uint8_t>(platform ? 2 : 0), Layer::Front, false, Shade::Normal,
                               static_cast<uint16_t>(platform ? 1 : 0)};
            // The sprite, with a dark outline.
            const bool inSprite = x >= sx && x < sx + kSprite && y >= sy && y < sy + kSprite;
            const bool edge = inSprite && (x == sx || y == sy || x == sx + kSprite - 1 || y == sy + kSprite - 1);
            f.sprites[i] = !inSprite ? 0 : edge ? 0xFF101010u : kSpriteColours[ram_[3] & 3];
            f.spriteInfo[i] = {static_cast<uint8_t>(inSprite ? 3 : 0), Layer::Sprite, false, Shade::Normal, 0};
            // Mixed: sprite over the front plane over the back one over the backdrop.
            Pixel info{0, Layer::Backdrop, false, Shade::Normal, 0};
            uint32_t c = backdrop;
            if (f.planeB[i]) c = f.planeB[i], info = f.planeBInfo[i];
            if (f.planeA[i]) c = f.planeA[i], info = f.planeAInfo[i];
            if (f.sprites[i]) c = f.sprites[i], info = f.spriteInfo[i];
            f.rgba[i] = c;
            f.info[i] = info;
        }
    }
}

void TestConsole::saveBlocks(StateBlocks& blocks) {
    blocks.add("TEST", [&](StateWriter& w) { w(ram_, phase_); });
}

bool TestConsole::loadBlock(uint32_t id, StateReader& r) {
    if (id == fourcc("TEST")) r(ram_, phase_);
    return r.ok();
}

}  // namespace plumnin
