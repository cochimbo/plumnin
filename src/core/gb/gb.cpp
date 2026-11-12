#include "gb.h"

#include <algorithm>
#include <cctype>

#include "../i18n.h"

namespace plumnin {

namespace {

constexpr uint16_t kTimerMask[4] = {0x200, 0x8, 0x20, 0x80};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

GameBoy::GameBoy() : frame_(std::make_unique<VideoFrame>()) {
    VideoFrame& f = *frame_;
    f.width = gb::kScreenWidth;
    f.height = gb::kScreenHeight;
    f.wideLeft = f.wideRight = 0;
    f.interlaced = false;
    f.aspectLines = gb::kScreenHeight;
    f.screenAspect = static_cast<float>(gb::kScreenWidth) / gb::kScreenHeight;
    ppu_.setFrame(frame_.get());
    apu_.setSampleRate(44100);
}

GameBoy::~GameBoy() = default;

bool GameBoy::insert(const std::vector<uint8_t>& rom, std::string* error) {
    gb::Cartridge cart;
    if (!cart.load(rom, error)) return false;
    cart_ = std::move(cart);
    color_ = cart_.colorGame();
    romCheats_.clear();
    ramCheats_.clear();
    powerCycle();
    return true;
}

void GameBoy::powerCycle() {
    wram_.fill(0);
    hram_.fill(0);
    reset();
}

void GameBoy::reset() {
    cart_.reset();
    ppu_.reset(color_);
    apu_.reset(color_);
    // The registers as the boot ROM leaves them.
    if (color_) {
        a_ = 0x11, f_ = 0x80, b_ = 0x00, c_ = 0x00, d_ = 0xFF, e_ = 0x56, h_ = 0x00, l_ = 0x0D;
        div_ = 0x1EA0;
    } else {
        a_ = 0x01, f_ = 0xB0, b_ = 0x00, c_ = 0x13, d_ = 0x00, e_ = 0xD8, h_ = 0x01, l_ = 0x4D;
        div_ = 0xABC8;
    }
    sp_ = 0xFFFE;
    pc_ = 0x0100;
    ime_ = eiDelay_ = halted_ = haltBug_ = stopped_ = false;
    breakpoint_ = false;
    wramBank_ = 1;
    if_ = 0xE1;
    ie_ = 0;
    p1_ = 0x00;
    sb_ = 0;
    sc_ = 0;
    tima_ = tma_ = 0;
    tac_ = 0;
    timaOverflow_ = timaReloaded_ = false;
    serialCycles_ = 0;
    doubleSpeed_ = false;
    key1_ = 0;
    dmaActive_ = dmaCopying_ = false;
    dmaDelay_ = dmaSource_ = dmaNextSource_ = dmaIndex_ = 0;
    dmaReg_ = 0xFF;
    hdmaSource_ = hdmaDest_ = 0;
    hdmaLength_ = 0x7F;
    hdmaActive_ = false;
    cycles_ = 0;
    serial_.clear();
}

bool GameBoy::timerBit(uint16_t div) const { return (tac_ & 4) && (div & kTimerMask[tac_ & 3]); }

void GameBoy::timaIncrement() {
    if (++tima_ == 0) timaOverflow_ = true;  // TMA is loaded one M-cycle later
}

void GameBoy::setDiv(uint16_t value) {
    const uint16_t old = div_;
    div_ = value;
    if (timerBit(old) && !timerBit(value)) timaIncrement();
    const uint16_t sound = doubleSpeed_ ? 0x2000 : 0x1000;
    if ((old & sound) && !(value & sound)) apu_.frameSequencerClock();
}

void GameBoy::tick() {
    ++cycles_;
    timaReloaded_ = false;
    if (timaOverflow_) {
        timaOverflow_ = false;
        tima_ = tma_;
        if_ |= 4;
        timaReloaded_ = true;
    }
    setDiv(static_cast<uint16_t>(div_ + 4));

    if ((sc_ & 0x81) == 0x81 && serialCycles_ > 0 && --serialCycles_ == 0) {
        sb_ = 0xFF;  // nobody on the other end of the cable
        sc_ &= 0x7F;
        if_ |= 8;
    }

    if (dmaCopying_) {
        uint16_t from = static_cast<uint16_t>(dmaSource_ << 8 | dmaIndex_);
        if (from >= 0xE000) from = static_cast<uint16_t>(from - 0x2000);
        ppu_.oam[dmaIndex_] = busRead(from);
        if (++dmaIndex_ == 0xA0) dmaActive_ = dmaCopying_ = false;
    }
    if (dmaDelay_ && --dmaDelay_ == 0) {
        dmaActive_ = dmaCopying_ = true;
        dmaSource_ = dmaNextSource_;
        dmaIndex_ = 0;
    }

    const int dots = doubleSpeed_ ? 2 : 4;
    ppu_.run(dots, if_);
    apu_.run(dots);
    cart_.tickClock(dots);
}

uint8_t GameBoy::busRead(uint16_t a) const {
    if (a < 0x8000) {
        const uint8_t v = cart_.readRom(a);
        for (const CheatPatch& p : romCheats_)
            if (p.address == a && (p.compare < 0 || p.compare == v)) return static_cast<uint8_t>(p.value);
        return v;
    }
    if (a < 0xA000) return ppu_.vram[color_ ? ppu_.vramBank : 0][a - 0x8000];
    if (a < 0xC000) return cart_.readRam(a);
    if (a < 0xD000) return wram_[a - 0xC000];
    if (a < 0xE000) return wram_[static_cast<size_t>(wramBank_) * 0x1000 + (a - 0xD000)];
    if (a < 0xFE00) return busRead(static_cast<uint16_t>(a - 0x2000));
    if (a < 0xFEA0) return dmaActive_ ? 0xFF : ppu_.oam[a - 0xFE00];
    if (a < 0xFF00) return color_ ? 0x00 : 0xFF;
    if (a < 0xFF80) return readIo(a);
    if (a < 0xFFFF) return hram_[a - 0xFF80];
    return ie_;
}

uint8_t GameBoy::peek(uint16_t address) const { return busRead(address); }

void GameBoy::busWrite(uint16_t a, uint8_t v) {
    if (a < 0x8000) cart_.write(a, v);
    else if (a < 0xA000) ppu_.vram[color_ ? ppu_.vramBank : 0][a - 0x8000] = v;
    else if (a < 0xC000) cart_.write(a, v);
    else if (a < 0xD000) wram_[a - 0xC000] = v;
    else if (a < 0xE000) wram_[static_cast<size_t>(wramBank_) * 0x1000 + (a - 0xD000)] = v;
    else if (a < 0xFE00) busWrite(static_cast<uint16_t>(a - 0x2000), v);
    else if (a < 0xFEA0) {
        if (!dmaActive_) ppu_.oam[a - 0xFE00] = v;
    } else if (a < 0xFF00) {
    } else if (a < 0xFF80) writeIo(a, v);
    else if (a < 0xFFFF) hram_[a - 0xFF80] = v;
    else ie_ = v;
}

uint8_t GameBoy::joypad() const {
    uint8_t low = 0x0F;
    if (!(p1_ & 0x10)) low &= static_cast<uint8_t>(~(buttons_ & 0x0F));
    if (!(p1_ & 0x20)) low &= static_cast<uint8_t>(~(buttons_ >> 4));
    return static_cast<uint8_t>(0xC0 | (p1_ & 0x30) | low);
}

void GameBoy::updateJoypad() {
    const uint8_t before = joypad();
    const PadState none{};
    const PadState& p = padTypes_[0] == PadType::None ? none : pads_[0];
    buttons_ = static_cast<uint8_t>((p.right ? 1 : 0) | (p.left ? 2 : 0) | (p.up ? 4 : 0) | (p.down ? 8 : 0) |
                                    (p.a ? 0x10 : 0) | (p.b ? 0x20 : 0) | (p.select ? 0x40 : 0) |
                                    (p.start ? 0x80 : 0));
    // A line going low raises the joypad interrupt (and wakes a STOP).
    if (before & ~joypad() & 0x0F) {
        if_ |= 0x10;
        stopped_ = false;
    }
}

uint8_t GameBoy::readIo(uint16_t a) const {
    if (a >= 0xFF10 && a < 0xFF40) return apu_.read(a);
    switch (a) {
        case 0xFF00: return joypad();
        case 0xFF01: return sb_;
        case 0xFF02: return static_cast<uint8_t>(sc_ | (color_ ? 0x7C : 0x7E));
        case 0xFF04: return static_cast<uint8_t>(div_ >> 8);
        case 0xFF05: return tima_;
        case 0xFF06: return tma_;
        case 0xFF07: return static_cast<uint8_t>(tac_ | 0xF8);
        case 0xFF0F: return static_cast<uint8_t>(if_ | 0xE0);
        case 0xFF46: return dmaReg_;
        default: break;
    }
    if ((a >= 0xFF40 && a <= 0xFF4B) || a == 0xFF4F || (a >= 0xFF68 && a <= 0xFF6C)) return ppu_.read(a);
    if (!color_) return 0xFF;
    switch (a) {
        case 0xFF4D: return static_cast<uint8_t>((doubleSpeed_ ? 0x80 : 0) | 0x7E | (key1_ & 1));
        case 0xFF55: return hdmaActive_ ? hdmaLength_ : static_cast<uint8_t>(0x80 | hdmaLength_);
        case 0xFF70: return static_cast<uint8_t>(0xF8 | wramBank_);
        default: return 0xFF;
    }
}

void GameBoy::hdmaBlock() {
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = busRead(hdmaSource_);
        ppu_.vram[ppu_.vramBank][hdmaDest_ & 0x1FFF] = v;
        hdmaSource_ = static_cast<uint16_t>(hdmaSource_ + 1);
        hdmaDest_ = static_cast<uint16_t>(((hdmaDest_ + 1) & 0x1FFF) | 0x8000);
    }
    // The processor waits 8 microseconds a block, at either speed.
    for (int i = 0; i < (doubleSpeed_ ? 16 : 8); ++i) tick();
}

void GameBoy::writeIo(uint16_t a, uint8_t v) {
    if (a >= 0xFF10 && a < 0xFF40) {
        apu_.write(a, v);
        return;
    }
    switch (a) {
        case 0xFF00: {
            const uint8_t before = joypad();
            p1_ = v & 0x30;
            if (before & ~joypad() & 0x0F) if_ |= 0x10;
            return;
        }
        case 0xFF01: sb_ = v; return;
        case 0xFF02:
            sc_ = v & (color_ ? 0x83 : 0x81);
            if ((sc_ & 0x81) == 0x81) {
                serial_ += static_cast<char>(sb_);
                if (serial_.size() > 65536) serial_.erase(0, 32768);
                serialCycles_ = color_ && (sc_ & 2) ? 32 : 1024;
                if (doubleSpeed_) serialCycles_ *= 2;
            }
            return;
        case 0xFF04: setDiv(0); return;
        case 0xFF05:
            if (timaReloaded_) return;  // the reload from TMA wins
            tima_ = v;
            timaOverflow_ = false;
            return;
        case 0xFF06:
            tma_ = v;
            if (timaReloaded_) tima_ = v;
            return;
        case 0xFF07: {
            const bool before = timerBit(div_);
            tac_ = v & 7;
            if (before && !timerBit(div_)) timaIncrement();
            return;
        }
        case 0xFF0F: if_ = v & 0x1F; return;
        case 0xFF46:
            dmaReg_ = v;
            dmaNextSource_ = v;
            dmaDelay_ = 2;
            return;
        default: break;
    }
    if ((a >= 0xFF40 && a <= 0xFF4B) || a == 0xFF4F || (a >= 0xFF68 && a <= 0xFF6C)) {
        ppu_.write(a, v, if_);
        return;
    }
    if (!color_) return;
    switch (a) {
        case 0xFF4D: key1_ = v & 1; break;
        case 0xFF51: hdmaSource_ = static_cast<uint16_t>((hdmaSource_ & 0x00FF) | v << 8); break;
        case 0xFF52: hdmaSource_ = static_cast<uint16_t>((hdmaSource_ & 0xFF00) | (v & 0xF0)); break;
        case 0xFF53: hdmaDest_ = static_cast<uint16_t>((hdmaDest_ & 0x00FF) | (v & 0x1F) << 8 | 0x8000); break;
        case 0xFF54: hdmaDest_ = static_cast<uint16_t>((hdmaDest_ & 0xFF00) | (v & 0xF0) | 0x8000); break;
        case 0xFF55:
            if (hdmaActive_ && !(v & 0x80)) {
                hdmaActive_ = false;  // an HBlank DMA stopped halfway
                return;
            }
            hdmaLength_ = v & 0x7F;
            if (v & 0x80) {
                hdmaActive_ = true;
                return;
            }
            // General purpose DMA: all of it now.
            for (int left = hdmaLength_; left >= 0; --left) {
                hdmaBlock();
                hdmaLength_ = static_cast<uint8_t>(left - 1) & 0x7F;
            }
            hdmaLength_ = 0x7F;
            break;
        case 0xFF70: wramBank_ = (v & 7) ? (v & 7) : 1; break;
        default: break;
    }
}

void GameBoy::applyRamCheats() {
    for (const CheatPatch& p : ramCheats_) {
        const uint16_t a = static_cast<uint16_t>(p.address);
        const unsigned bank = p.address >> 16;
        if (bank && color_ && a >= 0xD000 && a < 0xE000) wram_[bank * 0x1000 + (a - 0xD000)] = static_cast<uint8_t>(p.value);
        else if ((a >= 0xA000 && a < 0xE000) || (a >= 0xFF80 && a < 0xFFFF)) busWrite(a, static_cast<uint8_t>(p.value));
    }
}

void GameBoy::runFrame() {
    updateJoypad();
    applyRamCheats();
    ppu_.setDrawing(drawing_);
    // A frame is 70224 dots; give up after a few in case the screen never ends one.
    const uint64_t limit = cycles_ + static_cast<uint64_t>(gb::kDotsPerFrame);
    while (!ppu_.takeFrameDone() && cycles_ < limit) step();
    apu_.takeSamples(audio_.output());
    ++frames_;
    frame_->frameNumber = frames_;
}

CheatDecoder GameBoy::cheatDecoder() const { return decoder(); }

const char* GameBoy::cheatFormats() { return N_("Game Genie (ABC-DEF-GHI) o GameShark (01FF34C1)"); }

CheatDecoder GameBoy::decoder() {
    return [](const std::string& text, CheatPatch* out, std::string* error) {
        std::string c;
        for (char ch : text)
            if (ch != '-') c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (char ch : c)
            if (hexDigit(ch) < 0) {
                if (error) *error = tr("un código de Game Boy solo lleva cifras hexadecimales");
                return false;
            }
        auto hex = [&](size_t i) { return hexDigit(c[i]); };
        CheatPatch p;
        if (c.size() == 6 || c.size() == 9) {
            // Game Genie: ROM patch, with the byte it replaces in the long form.
            p.value = static_cast<uint16_t>(hex(0) << 4 | hex(1));
            p.address = static_cast<uint32_t>((hex(5) ^ 0xF) << 12 | hex(2) << 8 | hex(3) << 4 | hex(4));
            if (p.address >= 0x8000) {
                if (error) *error = tr("el código Game Genie apunta fuera de la ROM");
                return false;
            }
            if (c.size() == 9) {
                const int gi = hex(6) << 4 | hex(8);
                p.compare = (((gi >> 2) | (gi << 6)) & 0xFF) ^ 0xBA;
            }
        } else if (c.size() == 8) {
            // GameShark: type, value, address with its low byte first; written every frame.
            const int type = hex(0) << 4 | hex(1);
            if (type != 0x01 && (type & 0xE8) != 0x80) {
                if (error) *error = tr("tipo de código GameShark desconocido");
                return false;
            }
            p.value = static_cast<uint16_t>(hex(2) << 4 | hex(3));
            p.address = static_cast<uint32_t>(hex(6) << 12 | hex(7) << 8 | hex(4) << 4 | hex(5));
            if (type & 0x80) p.address |= static_cast<uint32_t>(type & 7) << 16;
            p.ram = true;
        } else {
            if (error) *error = tr("un código de Game Boy tiene 6 o 9 cifras (Game Genie) u 8 (GameShark)");
            return false;
        }
        if (out) *out = p;
        return true;
    };
}

void GameBoy::setCheats(const std::vector<CheatPatch>& patches) {
    romCheats_.clear();
    ramCheats_.clear();
    for (const CheatPatch& p : patches) (p.ram ? ramCheats_ : romCheats_).push_back(p);
}

std::vector<MemoryRegion> GameBoy::memoryMap() {
    // RetroAchievements' map of the Game Boy: the bus as the processor sees it, then the
    // Color's other WRAM banks and the cartridge's other RAM banks.
    std::vector<MemoryRegion> m;
    m.push_back({0x0000, const_cast<uint8_t*>(cart_.romBank0()), 0x4000});
    m.push_back({0x4000, const_cast<uint8_t*>(cart_.romBankN()), 0x4000});
    m.push_back({0x8000, ppu_.vram[color_ ? ppu_.vramBank : 0], 0x2000});
    if (cart_.hasRam()) m.push_back({0xA000, cart_.ramBank(), cart_.ramBankSize()});
    uint8_t* bank = &wram_[static_cast<size_t>(wramBank_) * 0x1000];
    m.push_back({0xC000, wram_.data(), 0x1000});
    m.push_back({0xD000, bank, 0x1000});
    m.push_back({0xE000, wram_.data(), 0x1000});
    m.push_back({0xF000, bank, 0xE00});
    m.push_back({0xFE00, ppu_.oam, 0xA0});
    m.push_back({0xFF80, hram_.data(), hram_.size()});
    m.push_back({0xFFFF, &ie_, 1});
    if (color_)
        for (size_t b = 2; b < 8; ++b) m.push_back({static_cast<uint32_t>(0x10000 + (b - 2) * 0x1000), &wram_[b * 0x1000], 0x1000});
    std::vector<uint8_t>& ram = cart_.ram();
    for (size_t b = 1; b < 16 && ram.size() > 0x2000 && (b + 1) * 0x2000 <= ram.size(); ++b)
        m.push_back({static_cast<uint32_t>(0x16000 + (b - 1) * 0x2000), &ram[b * 0x2000], 0x2000});
    return m;
}

void GameBoy::saveBlocks(StateBlocks& blocks) {
    blocks.add("GBCP", [&](StateWriter& w) {
        w(a_, f_, b_, c_, d_, e_, h_, l_, sp_, pc_, ime_, eiDelay_, halted_, haltBug_, stopped_);
    });
    blocks.add("GBMM", [&](StateWriter& w) {
        w(wram_, hram_, wramBank_, if_, ie_, p1_, sb_, sc_, div_, tima_, tma_, tac_, timaOverflow_, timaReloaded_,
          serialCycles_, doubleSpeed_, key1_, dmaActive_, dmaCopying_, dmaDelay_, dmaSource_, dmaNextSource_,
          dmaIndex_, dmaReg_, hdmaSource_, hdmaDest_, hdmaLength_, hdmaActive_, cycles_);
    });
    blocks.add("GBPP", [&](StateWriter& w) { ppu_.state(w); });
    blocks.add("GBAP", [&](StateWriter& w) { apu_.state(w); });
    blocks.add("GBCT", [&](StateWriter& w) { cart_.state(w); });
}

bool GameBoy::loadBlock(uint32_t id, StateReader& r) {
    if (id == fourcc("GBCP")) r(a_, f_, b_, c_, d_, e_, h_, l_, sp_, pc_, ime_, eiDelay_, halted_, haltBug_, stopped_);
    else if (id == fourcc("GBMM"))
        r(wram_, hram_, wramBank_, if_, ie_, p1_, sb_, sc_, div_, tima_, tma_, tac_, timaOverflow_, timaReloaded_,
          serialCycles_, doubleSpeed_, key1_, dmaActive_, dmaCopying_, dmaDelay_, dmaSource_, dmaNextSource_,
          dmaIndex_, dmaReg_, hdmaSource_, hdmaDest_, hdmaLength_, hdmaActive_, cycles_);
    else if (id == fourcc("GBPP")) ppu_.state(r);
    else if (id == fourcc("GBAP")) apu_.state(r);
    else if (id == fourcc("GBCT")) cart_.state(r);
    return r.ok();
}

}  // namespace plumnin
