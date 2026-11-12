#include "nes.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "../i18n.h"

namespace plumnin {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The Game Genie's sixteen letters, in the order of the nibbles they stand for.
constexpr const char* kGenieLetters = "APZLGITYEOXUKSVN";

int genieLetter(char c) {
    const char* p = std::strchr(kGenieLetters, c);
    return c && p ? static_cast<int>(p - kGenieLetters) : -1;
}

}  // namespace

Nes::Nes() : frame_(std::make_unique<VideoFrame>()) {
    VideoFrame& f = *frame_;
    f.width = 256;
    f.height = nes::kShownLines;
    f.wideLeft = f.wideRight = 0;
    f.interlaced = false;
    f.aspectLines = nes::kShownLines;
    f.screenAspect = 0;
    ppu_.setFrame(frame_.get());
    apu_.setSampleRate(44100);
}

Nes::~Nes() = default;

bool Nes::insert(const std::vector<uint8_t>& rom, std::string* error) {
    std::unique_ptr<nes::Mapper> cart = nes::Mapper::create(rom, error);
    if (!cart) return false;
    cart_ = std::move(cart);
    ppu_.setMapper(cart_.get());
    romCheats_.clear();
    ramCheats_.clear();
    setupRegion();
    powerCycle();
    return true;
}

Region Nes::effectiveRegion() const {
    if (region() == Region::Auto) return cart_ && cart_->info().pal ? Region::EuropePal : Region::UsaNtsc;
    return region();
}

void Nes::setupRegion() {
    pal_ = effectiveRegion() == Region::EuropePal;
    startClocks_ = endClocks_ = pal_ ? 8 : 6;
    ppuDivider_ = pal_ ? 5 : 4;
}

void Nes::regionChanged() {
    if (!cart_) return;
    const bool before = pal_;
    setupRegion();
    if (pal_ != before) powerCycle();
}

void Nes::powerCycle() {
    if (!cart_) return;
    ram_.fill(0);
    openBus_ = 0;
    strobe_ = false;
    shift_.fill(0);
    cart_->reset();
    ppu_.power(pal_);
    apu_.power(pal_);
    cpuReset(false);
}

void Nes::reset() {
    if (!cart_) return;
    ppu_.reset();
    apu_.reset();
    cpuReset(true);
}

void Nes::cpuReset(bool soft) {
    // The vector is read without clocking anything; then the processor spends eight
    // cycles before the first instruction (the APU acts as if 4017 were written).
    pc_ = static_cast<uint16_t>(cart_->cpuRead(0xFFFC, 0) | cart_->cpuRead(0xFFFD, 0) << 8);
    if (soft) {
        p_ |= I;
        sp_ = static_cast<uint8_t>(sp_ - 3);
    } else {
        a_ = x_ = y_ = 0;
        sp_ = 0xFD;
        p_ = I;
    }
    jammed_ = false;
    needNmi_ = prevNeedNmi_ = prevNmiLine_ = false;
    runIrq_ = prevRunIrq_ = false;
    needHalt_ = needDummyRead_ = dmcRunning_ = abortDmc_ = spriteDma_ = false;
    cycle_ = -1;
    master_ = startClocks_ + endClocks_;
    ppuClock_ = 0;
    for (int i = 0; i < 8; ++i) {
        startCycle(true);
        endCycle(true);
    }
}

void Nes::startCycle(bool forRead) {
    master_ += forRead ? startClocks_ - 1u : startClocks_ + 1u;
    ++cycle_;
    runPpu(master_ - 1);
    cart_->cpuClock();
    apu_.clock(static_cast<uint64_t>(cycle_));
    if (apu_.takeDmcRequest()) dmcRunning_ = needDummyRead_ = needHalt_ = true;
    if (apu_.takeDmcAbort()) stopDmc();
}

void Nes::endCycle(bool forRead) {
    master_ += forRead ? endClocks_ + 1u : endClocks_ - 1u;
    runPpu(master_ - 1);
    // The NMI is an edge, seen in the cycle after the one it happened in; the IRQ a
    // level, masked by I as it was at the end of the cycle.
    prevNeedNmi_ = needNmi_;
    const bool nmi = ppu_.nmi();
    if (!prevNmiLine_ && nmi) needNmi_ = true;
    prevNmiLine_ = nmi;
    prevRunIrq_ = runIrq_;
    runIrq_ = (apu_.irq() || cart_->irq()) && !(p_ & I);
}

void Nes::stopDmc() {
    if (!dmcRunning_) return;
    if (needHalt_) {
        // Called off before the halt cycle: the fetch never happens.
        dmcRunning_ = needDummyRead_ = needHalt_ = false;
    } else {
        abortDmc_ = true;
    }
}

uint8_t Nes::busRead(uint16_t a) {
    uint8_t v;
    if (a < 0x2000) {
        v = ram_[a & 0x7FF];
    } else if (a < 0x4000) {
        v = ppu_.read(a);
    } else if (a < 0x4020) {
        if (a == 0x4015) {
            // An internal register: the value does not reach the outside bus.
            return static_cast<uint8_t>((apu_.readStatus(static_cast<uint64_t>(cycle_)) & ~0x20) | (openBus_ & 0x20));
        }
        if (a == 0x4016 || a == 0x4017) v = static_cast<uint8_t>((openBus_ & 0xE0) | readPad(a & 1));
        else v = openBus_;
    } else {
        v = cart_->cpuRead(a, openBus_);
        if (a >= 0x8000)
            for (const CheatPatch& p : romCheats_)
                if (p.address == a && (p.compare < 0 || p.compare == v)) v = static_cast<uint8_t>(p.value);
    }
    openBus_ = v;
    return v;
}

void Nes::busWrite(uint16_t a, uint8_t v) {
    openBus_ = v;
    if (a < 0x2000) {
        ram_[a & 0x7FF] = v;
    } else if (a < 0x4000) {
        ppu_.write(a, v);
    } else if (a < 0x4020) {
        if (a == 0x4014) {
            spriteDma_ = needHalt_ = true;
            spriteDmaPage_ = v;
        } else if (a == 0x4016) {
            strobe_ = v & 1;
            if (strobe_) latchPads();
        } else if (a < 0x4018) {
            apu_.write(a, v, static_cast<uint64_t>(cycle_));
        }
    } else {
        cart_->cpuWrite(a, v);
    }
}

void Nes::latchPads() {
    for (int port = 0; port < 2; ++port) {
        const PadState& s = pads_[static_cast<size_t>(port)];
        shift_[static_cast<size_t>(port)] =
            static_cast<uint8_t>(s.a | s.b << 1 | s.select << 2 | s.start << 3 | s.up << 4 | s.down << 5 |
                                 s.left << 6 | s.right << 7);
    }
}

uint8_t Nes::readPad(int port) {
    if (padTypes_[static_cast<size_t>(port)] == PadType::None) return 0;
    if (strobe_) latchPads();
    uint8_t& s = shift_[static_cast<size_t>(port)];
    const uint8_t bit = s & 1;
    s = static_cast<uint8_t>(0x80 | s >> 1);  // an official pad answers 1 after its eight buttons
    return bit;
}

// A DMA read while the processor was halted on 4000-401F also reads the internal
// register the address bus still selects: a 4015 read clears the frame IRQ, a pad
// read loses a bit, and their values mix with the DMA's.
uint8_t Nes::dmaRead(uint16_t a, uint16_t& previous, bool internal) {
    uint8_t v;
    if (!internal) {
        v = a >= 0x4000 && a < 0x4020 ? openBus_ : busRead(a);
        previous = a;
        return v;
    }
    const uint16_t reg = static_cast<uint16_t>(0x4000 | (a & 0x1F));
    const bool same = reg == a;
    switch (reg) {
        case 0x4015:
            v = busRead(reg);
            if (!same) busRead(a);
            break;
        case 0x4016:
        case 0x4017: {
            v = pal_ || previous == reg ? openBus_ : busRead(reg);
            if (!same) {
                const uint8_t outside = busRead(a);
                v = static_cast<uint8_t>((outside & 0xE0) | (v & outside & 0x1F));
            }
            break;
        }
        default: v = busRead(a); break;
    }
    previous = reg;
    return v;
}

// The processor is halted on a read for the sprite DMA (513 or 514 cycles) or a DMC
// fetch (3 or 4): it repeats the read it was halted on, while the DMA unit reads on
// its "get" cycles and writes on its "put" cycles.
void Nes::dma(uint16_t address) {
    const bool internal = (address & 0xFFE0) == 0x4000;
    const bool pads = address == 0x4016 || address == 0x4017;
    // When the DMC reads the same register the processor was reading, /OE stays low
    // and the pad sees one read.
    const bool skipFirst = internal && dmcRunning_ && pads && (apu_.dmcAddress() & 0x1F) == (address & 0x1F);
    // The NES (not the Famicom) only clocks a pad once while halted on it; on PAL the
    // halted reads go elsewhere.
    const bool skipDummies = pal_ || pads;
    needHalt_ = false;

    startCycle(true);
    if (!skipFirst) busRead(address);
    endCycle(true);
    if (abortDmc_) {
        dmcRunning_ = abortDmc_ = false;
        if (!spriteDma_) {
            needDummyRead_ = false;
            return;
        }
    }

    uint16_t previous = address;
    uint16_t count = 0;
    uint8_t spriteAddress = 0, value = 0;
    auto begin = [&] {
        if (abortDmc_) {
            dmcRunning_ = abortDmc_ = needDummyRead_ = needHalt_ = false;
        } else if (needHalt_) {
            needHalt_ = false;
        } else if (needDummyRead_) {
            needDummyRead_ = false;
        }
        startCycle(true);
    };
    while (dmcRunning_ || spriteDma_) {
        const bool get = (cycle_ & 1) == 0;
        if (get) {
            if (dmcRunning_ && !needHalt_ && !needDummyRead_) {
                begin();
                value = dmaRead(apu_.dmcAddress(), previous, internal);
                endCycle(true);
                dmcRunning_ = abortDmc_ = false;
                apu_.dmcFill(value);
            } else if (spriteDma_) {
                begin();
                value = dmaRead(static_cast<uint16_t>(spriteDmaPage_ << 8 | spriteAddress), previous, internal);
                endCycle(true);
                ++spriteAddress;
                ++count;
            } else {
                begin();
                if (!skipDummies) busRead(address);
                endCycle(true);
            }
        } else if (spriteDma_ && (count & 1)) {
            begin();
            busWrite(0x2004, value);
            endCycle(true);
            if (++count == 0x200) spriteDma_ = false;
        } else {
            begin();
            if (!skipDummies) busRead(address);
            endCycle(true);
        }
    }
}

uint8_t Nes::peek(uint16_t a) const {
    if (a < 0x2000) return ram_[a & 0x7FF];
    if (a < 0x4000) return ppu_.peek(a);
    if (a == 0x4015) return apu_.peekStatus();
    if (a < 0x4020) return openBus_;
    if (a >= 0x6000 && a < 0x8000) {
        std::vector<uint8_t>& ram = cart_->prgRam();
        return ram.empty() ? openBus_ : ram[(a - 0x6000u) % ram.size()];
    }
    return a >= 0x8000 ? cart_->prgByte(a) : openBus_;
}

std::string Nes::screenText() const {
    std::string text;
    if (!cart_) return text;
    const size_t page = static_cast<size_t>(cart_->nametable(0x2000)) * 0x400;
    for (int row = 0; row < 30; ++row) {
        std::string line;
        for (int col = 0; col < 32; ++col) {
            const uint8_t t = ppu_.vram[page + static_cast<size_t>(row * 32 + col)];
            line += t >= 0x20 && t < 0x7F ? static_cast<char>(t) : ' ';
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) text += line + "\n";
    }
    return text;
}

void Nes::applyRamCheats() {
    for (const CheatPatch& p : ramCheats_) {
        const uint16_t a = static_cast<uint16_t>(p.address);
        if (a < 0x2000) ram_[a & 0x7FF] = static_cast<uint8_t>(p.value);
        else if (a >= 0x6000 && a < 0x8000) cart_->cpuWrite(a, static_cast<uint8_t>(p.value));
    }
}

void Nes::runFrame() {
    if (!cart_) return;
    applyRamCheats();
    ppu_.setDrawing(drawing_);
    // A frame is about 29781 cycles; give up after two in case one never ends.
    const int64_t limit = cycle_ + 60000;
    while (!ppu_.takeFrameDone() && cycle_ < limit) step();
    apu_.takeSamples(audio_.output());
    ++frames_;
    frame_->frameNumber = frames_;
}

std::vector<uint8_t> Nes::saveData() const {
    if (!hasSaveData()) return {};
    return cart_->prgRam();
}

void Nes::loadSaveData(const std::vector<uint8_t>& data) {
    if (!cart_) return;
    std::vector<uint8_t>& ram = cart_->prgRam();
    std::copy_n(data.begin(), std::min(data.size(), ram.size()), ram.begin());
}

const char* Nes::cheatFormats() { return N_("Game Genie (SXIOPO o YEUZUGAA) o Pro Action Replay (0075:09)"); }

CheatDecoder Nes::decoder() {
    return [](const std::string& text, CheatPatch* out, std::string* error) {
        std::string c;
        for (char ch : text)
            if (ch != '-' && ch != ' ') c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        CheatPatch p;
        const size_t colon = c.find(':');
        bool genie = (c.size() == 6 || c.size() == 8) && colon == std::string::npos;
        for (char ch : c)
            if (genieLetter(ch) < 0) genie = false;
        if (genie) {
            int n[8] = {};
            for (size_t i = 0; i < c.size(); ++i) n[i] = genieLetter(c[i]);
            p.address = 0x8000u + static_cast<uint32_t>(((n[3] & 7) << 12) | ((n[5] & 7) << 8) | ((n[4] & 8) << 8) |
                                                        ((n[2] & 7) << 4) | ((n[1] & 8) << 4) | (n[4] & 7) | (n[3] & 8));
            if (c.size() == 6) {
                p.value = static_cast<uint16_t>(((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[5] & 8));
            } else {
                p.value = static_cast<uint16_t>(((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | (n[7] & 8));
                p.compare = ((n[7] & 7) << 4) | ((n[6] & 8) << 4) | (n[6] & 7) | (n[5] & 8);
            }
        } else {
            // Pro Action Replay: an address and the byte to keep there, AAAA:VV or AAAAVV.
            std::string hex = c;
            if (colon != std::string::npos) hex.erase(colon, 1);
            const bool ok = (colon == std::string::npos || colon == 4) && hex.size() == 6 &&
                            std::all_of(hex.begin(), hex.end(), [](char ch) { return hexDigit(ch) >= 0; });
            if (!ok) {
                if (error)
                    *error = tr("un código de NES es un Game Genie de 6 u 8 letras o un Pro Action Replay (0075:09)");
                return false;
            }
            uint32_t v = 0;
            for (char ch : hex) v = v << 4 | static_cast<uint32_t>(hexDigit(ch));
            p.address = v >> 8;
            p.value = static_cast<uint16_t>(v & 0xFF);
            if (p.address >= 0x8000) {
                // Into the ROM: a patch of what it answers there.
            } else if (p.address < 0x2000 || p.address >= 0x6000) {
                p.ram = true;
            } else {
                if (error) *error = tr("el código apunta a los registros del NES, no a su memoria");
                return false;
            }
        }
        if (out) *out = p;
        return true;
    };
}

void Nes::setCheats(const std::vector<CheatPatch>& patches) {
    romCheats_.clear();
    ramCheats_.clear();
    for (const CheatPatch& p : patches) (p.ram ? ramCheats_ : romCheats_).push_back(p);
}

std::vector<MemoryRegion> Nes::memoryMap() {
    // RetroAchievements' map of the NES: the 2 KB of RAM and its mirrors, then the
    // cartridge's RAM at 6000.
    std::vector<MemoryRegion> m;
    for (uint32_t mirror = 0; mirror < 0x2000; mirror += 0x800) m.push_back({mirror, ram_.data(), ram_.size()});
    if (cart_ && !cart_->prgRam().empty())
        m.push_back({0x6000, cart_->prgRam().data(), std::min<size_t>(0x2000, cart_->prgRam().size())});
    return m;
}

void Nes::saveBlocks(StateBlocks& blocks) {
    blocks.add("NSCP", [&](StateWriter& w) {
        w(a_, x_, y_, sp_, p_, pc_, jammed_, needNmi_, prevNeedNmi_, prevNmiLine_, runIrq_, prevRunIrq_, cycle_, master_,
          ppuClock_, needHalt_, needDummyRead_, dmcRunning_, abortDmc_, spriteDma_, spriteDmaPage_, pal_);
    });
    blocks.add("NSMM", [&](StateWriter& w) { w(ram_, openBus_, strobe_, shift_); });
    blocks.add("NSPP", [&](StateWriter& w) { ppu_.state(w); });
    blocks.add("NSAP", [&](StateWriter& w) { apu_.state(w); });
    blocks.add("NSCT", [&](StateWriter& w) { cart_->save(w); });
}

bool Nes::loadBlock(uint32_t id, StateReader& r) {
    if (id == fourcc("NSCP")) {
        r(a_, x_, y_, sp_, p_, pc_, jammed_, needNmi_, prevNeedNmi_, prevNmiLine_, runIrq_, prevRunIrq_, cycle_, master_,
          ppuClock_, needHalt_, needDummyRead_, dmcRunning_, abortDmc_, spriteDma_, spriteDmaPage_, pal_);
        startClocks_ = endClocks_ = pal_ ? 8 : 6;
        ppuDivider_ = pal_ ? 5 : 4;
    } else if (id == fourcc("NSMM")) {
        r(ram_, openBus_, strobe_, shift_);
    } else if (id == fourcc("NSPP")) {
        ppu_.state(r);
    } else if (id == fourcc("NSAP")) {
        apu_.state(r);
    } else if (id == fourcc("NSCT")) {
        cart_->load(r);
    }
    return r.ok();
}

}  // namespace plumnin
