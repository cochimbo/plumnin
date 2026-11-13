#include "snes.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "../i18n.h"
#include "snes_necdsp.h"

namespace plumnin {

namespace {

constexpr int kLineClocks = 1364;
constexpr int kRefreshAt = 536, kRefreshClocks = 40;  // the WRAM's refresh halts the processor
constexpr int kHdmaAt = 1104;                          // dot 276, the start of hblank

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The SNES Game Genie writes its sixteen hex digits in its own order.
constexpr const char* kGenieDigits = "DF4709156BC8A23E";

}  // namespace

Snes::Snes() : frame_(std::make_unique<VideoFrame>()) {
    VideoFrame& f = *frame_;
    f.width = 256;
    f.height = 224;
    f.aspectLines = 224;
    f.screenAspect = 0;
    ppu_.setFrame(frame_.get());
    cpu_.setBus(this);
    apu_.setSampleRate(44100);
}

Snes::~Snes() = default;

bool Snes::insert(const std::vector<uint8_t>& rom, std::string* error) {
    snes::Cart cart;
    if (!cart.load(rom, error)) return false;
    cart_ = std::move(cart);
    loaded_ = true;
    romCheats_.clear();
    ramCheats_.clear();
    loadNotice_.clear();
    setupCoprocessor();
    setupRegion();
    powerCycle();
    return true;
}

void Snes::setupCoprocessor() {
    using snes::Chip;
    using snes::NecDsp;
    copro_.reset();
    const snes::CartInfo& info = cart_.info();
    std::vector<const char*> files;
    NecDsp::Model model = NecDsp::Model::Upd7725;
    double hz = 7600000;
    switch (info.chip) {
        case Chip::None: return;
        case Chip::Dsp1:
            // Most games have the DSP-1B; Pilotwings' demo needs the first DSP-1.
            files = info.title.find("PILOTWINGS") != std::string::npos ? std::vector<const char*>{"dsp1.rom", "dsp1b.rom"}
                                                                       : std::vector<const char*>{"dsp1b.rom", "dsp1.rom"};
            break;
        case Chip::Dsp2: files = {"dsp2.rom"}; break;
        case Chip::Dsp3: files = {"dsp3.rom"}; break;
        case Chip::Dsp4: files = {"dsp4.rom"}; break;
        case Chip::St010:
            files = {"st010.rom"};
            model = NecDsp::Model::Upd96050;
            hz = 11000000;
            break;
        case Chip::St011:
            files = {"st011.rom"};
            model = NecDsp::Model::Upd96050;
            hz = 15000000;
            break;
        default:
            loadNotice_ = trf("Este juego usa el chip %s, que plumnin todavía no emula: puede que no funcione.",
                              info.coprocessor.c_str());
            return;
    }
    NecDsp::Map map = NecDsp::Map::LoRom;
    if (model == NecDsp::Model::Upd96050) map = NecDsp::Map::St01x;
    else if (info.mapping != snes::Mapping::LoRom) map = NecDsp::Map::HiRom;
    else if (cart_.rom().size() > 0x100000) map = NecDsp::Map::LoRomHigh;
    else if (info.ramSize) map = NecDsp::Map::LoRomRam;
    for (const char* name : files) {
        const std::vector<uint8_t> firmware = findFirmware(name);
        if (firmware.empty()) continue;
        auto dsp = std::make_unique<NecDsp>();
        if (!dsp->setFirmware(model, map, firmware, hz)) continue;
        copro_ = std::move(dsp);
        return;
    }
    loadNotice_ = trf(
        "Este juego necesita el programa del chip %s (el archivo %s), que no viene con plumnin: ponlo junto al juego o "
        "en la carpeta «firmware» de plumnin.",
        info.coprocessor.c_str(), files.front());
}

Region Snes::effectiveRegion() const {
    if (region() == Region::Auto) return cart_.info().pal ? Region::EuropePal : Region::UsaNtsc;
    return region();
}

void Snes::setupRegion() {
    pal_ = effectiveRegion() == Region::EuropePal;
    apu_.setMasterRate(pal_ ? 21281370.0 : 21477272.0);
    if (copro_) copro_->setMasterRate(pal_ ? 21281370.0 : 21477272.0);
}

void Snes::regionChanged() {
    if (!loaded_) return;
    const bool before = pal_;
    setupRegion();
    if (pal_ != before) powerCycle();
}

void Snes::powerCycle() {
    if (!loaded_) return;
    wram_.fill(0);
    ppu_.power();
    apu_.power();
    if (copro_) copro_->power();
    reset();
}

void Snes::reset() {
    if (!loaded_) return;
    wmadd_ = 0;
    mdr_ = 0;
    nmitimen_ = 0;
    wrio_ = 0xFF;
    wrmpya_ = 0xFF;
    wrmpyb_ = 0;
    wrdiv_ = 0xFFFF;
    rddiv_ = rdmpy_ = 0;
    htime_ = vtime_ = 0x1FF;
    memsel_ = hdmaen_ = 0;
    rdnmi_ = timeup_ = false;
    autoJoyBusyUntil_ = -1;
    joy_.fill(0);
    joyStrobe_ = false;
    joyShift_.fill(0);
    for (Channel& c : dma_) c = Channel{};
    ppu_.reset();
    apu_.reset();
    if (copro_) copro_->reset();
    master_ = 0;
    hc_ = 0;
    line_ = 0;
    refreshed_ = hdmaDoneLine_ = irqDoneLine_ = false;
    startLine();
    cpu_.power();
    frameDone_ = false;
}

int Snes::speed(uint32_t address) const {
    const uint32_t bank = address >> 16, a = address & 0xFFFF;
    if (!(bank & 0x40)) {
        if (a < 0x2000) return 8;
        if (a < 0x4000) return 6;
        if (a < 0x4200) return 12;
        if (a < 0x6000) return 6;
        if (a < 0x8000) return 8;
        return (bank & 0x80) && (memsel_ & 1) ? 6 : 8;
    }
    return (bank & 0x80) && (memsel_ & 1) ? 6 : 8;
}

int Snes::nextEvent() const {
    int next = kLineClocks;
    if (!refreshed_ && hc_ <= kRefreshAt) next = std::min(next, kRefreshAt);
    if (!hdmaDoneLine_ && hdmaen_ && line_ < ppu_.visibleLines() && hc_ <= kHdmaAt) next = std::min(next, kHdmaAt);
    if (!irqDoneLine_) {
        const int mode = (nmitimen_ >> 4) & 3;
        int at = -1;
        if (mode == 2 && line_ == vtime_) at = 10;
        else if ((mode == 1 || (mode == 3 && line_ == vtime_)) && htime_ <= 339) at = htime_ * 4 + 14;
        if (at >= hc_) next = std::min(next, at);
    }
    return next;
}

void Snes::advance(int clocks) {
    master_ += static_cast<uint64_t>(clocks);
    apu_.addTime(static_cast<uint32_t>(clocks));
    if (copro_) copro_->addTime(static_cast<uint32_t>(clocks));
    int target = hc_ + clocks;
    for (;;) {
        const int at = nextEvent();
        if (at > target) break;
        hc_ = at;
        if (at == kLineClocks) {
            target -= kLineClocks;
            hc_ = 0;
            ++line_;
            if (line_ >= (pal_ ? 312 : 262)) line_ = 0;
            refreshed_ = hdmaDoneLine_ = irqDoneLine_ = false;
            startLine();
            continue;
        }
        int stall = 0;
        if (at == kRefreshAt && !refreshed_) {
            refreshed_ = true;
            stall = kRefreshClocks;
        } else if (at == kHdmaAt && !hdmaDoneLine_ && hdmaen_ && line_ < ppu_.visibleLines()) {
            hdmaDoneLine_ = true;
            const uint64_t before = master_;
            hdmaRun();
            stall = static_cast<int>(master_ - before);
            master_ = before;
        } else {
            irqDoneLine_ = true;
            timeup_ = true;
            updateIrq();
        }
        if (stall) {
            master_ += static_cast<uint64_t>(stall);
            apu_.addTime(static_cast<uint32_t>(stall));
            if (copro_) copro_->addTime(static_cast<uint32_t>(stall));
            target += stall;
        }
    }
    hc_ = target;
}

void Snes::startLine() {
    ppu_.setBeam(line_, 0);
    const int visible = ppu_.visibleLines();
    if (line_ == 0) {
        rdnmi_ = false;
        ppu_.startFrame(pal_);
        hdmaSetup();
        frameDone_ = true;
    } else if (line_ <= visible) {
        ppu_.drawLine(line_, drawing_);
    }
    if (line_ == visible + 1) {
        ppu_.startVblank();
        rdnmi_ = true;
        if (nmitimen_ & 0x80) cpu_.raiseNmi();
        if (nmitimen_ & 1) autoJoypad();
    }
}

void Snes::updateIrq() { cpu_.setIrq(timeup_); }

uint16_t Snes::padBits(int port) const {
    if (padTypes_[static_cast<size_t>(port)] == PadType::None) return 0;
    const PadState& s = pads_[static_cast<size_t>(port)];
    return static_cast<uint16_t>(s.b << 15 | s.y << 14 | s.select << 13 | s.start << 12 | s.up << 11 | s.down << 10 |
                                 s.left << 9 | s.right << 8 | s.a << 7 | s.x << 6 | s.l << 5 | s.r << 4);
}

void Snes::autoJoypad() {
    joy_[0] = padBits(0);
    joy_[1] = padBits(1);
    joy_[2] = joy_[3] = 0;
    joyShift_.fill(0xFFFF);  // what the automatic read left in the shift registers
    autoJoyBusyUntil_ = line_ + 3;
}

uint8_t Snes::read(uint32_t address) {
    advance(speed(address));
    mdr_ = busRead(address);
    return mdr_;
}

void Snes::write(uint32_t address, uint8_t value) {
    advance(speed(address));
    mdr_ = value;
    busWrite(address, value);
}

uint8_t Snes::busRead(uint32_t address) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if ((bank & 0xFE) == 0x7E) return wram_[address & 0x1FFFF];
    if (!(bank & 0x40)) {
        if (a < 0x2000) return wram_[a];
        if (a < 0x6000) {
            if ((a & 0xFF00) == 0x2100) return bRead(static_cast<uint8_t>(a));
            if (a == 0x4016) return static_cast<uint8_t>((mdr_ & 0xFC) | ioRead(a));
            if (a == 0x4017) return static_cast<uint8_t>((mdr_ & 0xE0) | 0x1C | ioRead(a));
            if (a >= 0x4200 && a < 0x4220) return ioRead(a);
            if (a >= 0x4300 && a < 0x4380) return dmaRegRead(a);
            return mdr_;
        }
    }
    uint8_t v;
    if (copro_ && copro_->read(address, v)) return v;
    if (!cart_.read(address, v)) return mdr_;
    if (!romCheats_.empty()) {
        const int64_t offset = cart_.romOffset(address);
        for (const CheatPatch& p : romCheats_)
            if (offset >= 0 && cart_.romOffset(p.address) == offset && (p.compare < 0 || p.compare == v))
                v = static_cast<uint8_t>(p.value);
    }
    return v;
}

void Snes::busWrite(uint32_t address, uint8_t value) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if ((bank & 0xFE) == 0x7E) {
        wram_[address & 0x1FFFF] = value;
        return;
    }
    if (!(bank & 0x40)) {
        if (a < 0x2000) {
            wram_[a] = value;
            return;
        }
        if (a < 0x6000) {
            if ((a & 0xFF00) == 0x2100) bWrite(static_cast<uint8_t>(a), value);
            else if (a == 0x4016 || (a >= 0x4200 && a < 0x4220)) ioWrite(a, value);
            else if (a >= 0x4300 && a < 0x4380) dmaRegWrite(a, value);
            return;
        }
    }
    if (copro_ && copro_->write(address, value)) return;
    cart_.write(address, value);
}

uint8_t Snes::bRead(uint8_t reg) {
    if (reg < 0x40) {
        ppu_.setBeam(line_, hc_ >> 2);
        return ppu_.read(reg, mdr_);
    }
    if (reg < 0x80) return apu_.readPort(reg & 3);
    if (reg == 0x80) {
        const uint8_t v = wram_[wmadd_];
        wmadd_ = (wmadd_ + 1) & 0x1FFFF;
        return v;
    }
    return mdr_;
}

void Snes::bWrite(uint8_t reg, uint8_t value) {
    if (reg < 0x40) {
        ppu_.setBeam(line_, hc_ >> 2);
        ppu_.write(reg, value);
    } else if (reg < 0x80) {
        apu_.writePort(reg & 3, value);
    } else if (reg == 0x80) {
        wram_[wmadd_] = value;
        wmadd_ = (wmadd_ + 1) & 0x1FFFF;
    } else if (reg == 0x81) {
        wmadd_ = (wmadd_ & 0x1FF00) | value;
    } else if (reg == 0x82) {
        wmadd_ = (wmadd_ & 0x100FF) | static_cast<uint32_t>(value) << 8;
    } else if (reg == 0x83) {
        wmadd_ = (wmadd_ & 0x0FFFF) | static_cast<uint32_t>(value & 1) << 16;
    }
}

uint8_t Snes::ioRead(uint16_t a) {
    switch (a) {
        case 0x4016:
        case 0x4017: {
            const int port = a & 1;
            if (joyStrobe_) joyShift_[static_cast<size_t>(port)] = padBits(port);
            uint16_t& s = joyShift_[static_cast<size_t>(port)];
            const uint8_t bit = static_cast<uint8_t>(s >> 15);
            s = static_cast<uint16_t>(s << 1 | 1);  // an official pad answers 1 after its sixteen bits
            return bit;
        }
        case 0x4210: {
            const uint8_t v = static_cast<uint8_t>((rdnmi_ ? 0x80 : 0) | (mdr_ & 0x70) | 0x02);
            rdnmi_ = false;
            return v;
        }
        case 0x4211: {
            const uint8_t v = static_cast<uint8_t>((timeup_ ? 0x80 : 0) | (mdr_ & 0x7F));
            timeup_ = false;
            updateIrq();
            return v;
        }
        case 0x4212: {
            const bool vblank = line_ > ppu_.visibleLines();
            const bool hblank = hc_ < 4 || hc_ >= 1096;
            const bool busy = autoJoyBusyUntil_ >= 0 && line_ >= ppu_.visibleLines() + 1 && line_ < autoJoyBusyUntil_;
            return static_cast<uint8_t>((vblank ? 0x80 : 0) | (hblank ? 0x40 : 0) | (busy ? 1 : 0) | (mdr_ & 0x3E));
        }
        case 0x4213: return wrio_;
        case 0x4214: return static_cast<uint8_t>(rddiv_);
        case 0x4215: return static_cast<uint8_t>(rddiv_ >> 8);
        case 0x4216: return static_cast<uint8_t>(rdmpy_);
        case 0x4217: return static_cast<uint8_t>(rdmpy_ >> 8);
        default:
            if (a >= 0x4218 && a < 0x4220) {
                const uint16_t j = joy_[static_cast<size_t>((a - 0x4218) >> 1)];
                return static_cast<uint8_t>(a & 1 ? j >> 8 : j);
            }
            return mdr_;
    }
}

void Snes::ioWrite(uint16_t a, uint8_t v) {
    switch (a) {
        case 0x4016:
            joyStrobe_ = v & 1;
            if (joyStrobe_) joyShift_ = {padBits(0), padBits(1)};
            break;
        case 0x4200: {
            const uint8_t before = nmitimen_;
            nmitimen_ = v;
            if (!(v & 0x30)) timeup_ = false;
            // Turning NMIs on in vblank, before 4210 was read, makes one at once.
            if (!(before & 0x80) && (v & 0x80) && rdnmi_) cpu_.raiseNmi();
            updateIrq();
            break;
        }
        case 0x4201:
            if ((wrio_ & 0x80) && !(v & 0x80)) {
                ppu_.setBeam(line_, hc_ >> 2);
                ppu_.latchCounters();
            }
            wrio_ = v;
            break;
        case 0x4202: wrmpya_ = v; break;
        case 0x4203:
            wrmpyb_ = v;
            rdmpy_ = static_cast<uint16_t>(wrmpya_ * v);
            rddiv_ = static_cast<uint16_t>(v << 8 | wrmpya_);
            break;
        case 0x4204: wrdiv_ = static_cast<uint16_t>((wrdiv_ & 0xFF00) | v); break;
        case 0x4205: wrdiv_ = static_cast<uint16_t>((wrdiv_ & 0x00FF) | v << 8); break;
        case 0x4206:
            if (!v) {
                rddiv_ = 0xFFFF;
                rdmpy_ = wrdiv_;
            } else {
                rddiv_ = static_cast<uint16_t>(wrdiv_ / v);
                rdmpy_ = static_cast<uint16_t>(wrdiv_ % v);
            }
            break;
        case 0x4207: htime_ = static_cast<uint16_t>((htime_ & 0x100) | v); break;
        case 0x4208: htime_ = static_cast<uint16_t>((htime_ & 0x0FF) | (v & 1) << 8); break;
        case 0x4209: vtime_ = static_cast<uint16_t>((vtime_ & 0x100) | v); break;
        case 0x420A: vtime_ = static_cast<uint16_t>((vtime_ & 0x0FF) | (v & 1) << 8); break;
        case 0x420B: runDma(v); break;
        case 0x420C: hdmaen_ = v; break;
        case 0x420D: memsel_ = v & 1; break;
        default: break;
    }
}

uint8_t Snes::dmaRegRead(uint16_t a) const {
    const Channel& c = dma_[(a >> 4) & 7];
    switch (a & 0xF) {
        case 0x0: return c.dmap;
        case 0x1: return c.bbad;
        case 0x2: return static_cast<uint8_t>(c.a1t);
        case 0x3: return static_cast<uint8_t>(c.a1t >> 8);
        case 0x4: return c.a1b;
        case 0x5: return static_cast<uint8_t>(c.das);
        case 0x6: return static_cast<uint8_t>(c.das >> 8);
        case 0x7: return c.dasb;
        case 0x8: return static_cast<uint8_t>(c.a2a);
        case 0x9: return static_cast<uint8_t>(c.a2a >> 8);
        case 0xA: return c.ntrl;
        case 0xB:
        case 0xF: return c.unused;
        default: return mdr_;
    }
}

void Snes::dmaRegWrite(uint16_t a, uint8_t v) {
    Channel& c = dma_[(a >> 4) & 7];
    switch (a & 0xF) {
        case 0x0: c.dmap = v; break;
        case 0x1: c.bbad = v; break;
        case 0x2: c.a1t = static_cast<uint16_t>((c.a1t & 0xFF00) | v); break;
        case 0x3: c.a1t = static_cast<uint16_t>((c.a1t & 0x00FF) | v << 8); break;
        case 0x4: c.a1b = v; break;
        case 0x5: c.das = static_cast<uint16_t>((c.das & 0xFF00) | v); break;
        case 0x6: c.das = static_cast<uint16_t>((c.das & 0x00FF) | v << 8); break;
        case 0x7: c.dasb = v; break;
        case 0x8: c.a2a = static_cast<uint16_t>((c.a2a & 0xFF00) | v); break;
        case 0x9: c.a2a = static_cast<uint16_t>((c.a2a & 0x00FF) | v << 8); break;
        case 0xA: c.ntrl = v; break;
        case 0xB:
        case 0xF: c.unused = v; break;
        default: break;
    }
}

// The A bus as the DMA sees it: it cannot reach the B bus through it, nor the DMA's
// own registers.
uint8_t Snes::dmaRead(uint32_t address) {
    const uint32_t bank = address >> 16;
    const uint16_t a = static_cast<uint16_t>(address);
    if (!(bank & 0x40) && ((a & 0xFF00) == 0x2100 || (a >= 0x4300 && a < 0x4380) || a == 0x420B || a == 0x420C))
        return mdr_;
    return busRead(address);
}

namespace {

// Which B bus registers each transfer mode writes, byte after byte.
constexpr uint8_t kPattern[8][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
                                    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};
constexpr int kUnitLength[8] = {1, 2, 2, 4, 4, 4, 2, 4};

}  // namespace

void Snes::runDma(uint8_t channels) {
    if (!channels) return;
    advance(8);
    for (int ch = 0; ch < 8; ++ch) {
        if (!(channels & (1 << ch))) continue;
        Channel& c = dma_[static_cast<size_t>(ch)];
        advance(8);
        const int mode = c.dmap & 7;
        const bool toA = c.dmap & 0x80;
        const int step = c.dmap & 0x08 ? 0 : c.dmap & 0x10 ? -1 : 1;
        int i = 0;
        do {
            const uint8_t reg = static_cast<uint8_t>(c.bbad + kPattern[mode][i & 3]);
            const uint32_t address = static_cast<uint32_t>(c.a1b) << 16 | c.a1t;
            advance(8);
            if (toA) {
                const uint8_t v = bRead(reg);
                mdr_ = v;
                busWrite(address, v);
            } else {
                const uint8_t v = dmaRead(address);
                mdr_ = v;
                bWrite(reg, v);
            }
            c.a1t = static_cast<uint16_t>(c.a1t + step);
            --c.das;
            ++i;
        } while (c.das != 0);
    }
}

void Snes::hdmaReload(Channel& c, int& cost) {
    if ((c.ntrl & 0x7F) != 0) return;
    c.ntrl = busRead(static_cast<uint32_t>(c.a1b) << 16 | c.a2a);
    c.a2a = static_cast<uint16_t>(c.a2a + 1);
    cost += 8;
    c.hdmaDone = c.ntrl == 0;
    c.hdmaDo = !c.hdmaDone;
    if ((c.dmap & 0x40) && !c.hdmaDone) {
        const uint8_t lo = busRead(static_cast<uint32_t>(c.a1b) << 16 | c.a2a);
        const uint8_t hi = busRead(static_cast<uint32_t>(c.a1b) << 16 | static_cast<uint16_t>(c.a2a + 1));
        c.das = static_cast<uint16_t>(lo | hi << 8);
        c.a2a = static_cast<uint16_t>(c.a2a + 2);
        cost += 16;
    }
}

void Snes::hdmaSetup() {
    int cost = 0;
    for (int ch = 0; ch < 8; ++ch) {
        Channel& c = dma_[static_cast<size_t>(ch)];
        c.hdmaDone = false;
        c.hdmaDo = false;
        if (!(hdmaen_ & (1 << ch))) continue;
        c.a2a = c.a1t;
        c.ntrl = 0;
        hdmaReload(c, cost);
    }
}

void Snes::hdmaRun() {
    int cost = 18;
    for (int ch = 0; ch < 8; ++ch) {
        Channel& c = dma_[static_cast<size_t>(ch)];
        if (!(hdmaen_ & (1 << ch)) || c.hdmaDone) continue;
        cost += 8;
        if (!c.hdmaDo) continue;
        const int mode = c.dmap & 7;
        const bool indirect = c.dmap & 0x40;
        const bool toA = c.dmap & 0x80;
        for (int i = 0; i < kUnitLength[mode]; ++i) {
            const uint8_t reg = static_cast<uint8_t>(c.bbad + kPattern[mode][i]);
            uint32_t address;
            if (indirect) {
                address = static_cast<uint32_t>(c.dasb) << 16 | c.das;
                c.das = static_cast<uint16_t>(c.das + 1);
            } else {
                address = static_cast<uint32_t>(c.a1b) << 16 | c.a2a;
                c.a2a = static_cast<uint16_t>(c.a2a + 1);
            }
            if (toA) busWrite(address, bRead(reg));
            else bWrite(reg, dmaRead(address));
            cost += 8;
        }
    }
    for (int ch = 0; ch < 8; ++ch) {
        Channel& c = dma_[static_cast<size_t>(ch)];
        if (!(hdmaen_ & (1 << ch)) || c.hdmaDone) continue;
        --c.ntrl;
        c.hdmaDo = c.ntrl & 0x80;
        hdmaReload(c, cost);
    }
    master_ += static_cast<uint64_t>(cost);
}

uint8_t Snes::peek(uint32_t address) {
    const uint32_t bank = (address >> 16) & 0xFF;
    const uint16_t a = static_cast<uint16_t>(address);
    if ((bank & 0xFE) == 0x7E) return wram_[address & 0x1FFFF];
    if (!(bank & 0x40) && a < 0x2000) return wram_[a];
    if (!(bank & 0x40) && a < 0x6000) return 0;
    uint8_t v = 0;
    cart_.read(address & 0xFFFFFF, v);
    return v;
}

std::string Snes::screenText(int first) const {
    std::string text;
    const uint16_t base = ppu_.mapBase(0);
    for (int row = 0; row < 28; ++row) {
        std::string line;
        for (int col = 0; col < 32; ++col) {
            const int t = (ppu_.vramWord(static_cast<uint16_t>(base + row * 32 + col)) & 0x3FF) - first + 0x20;
            line += t >= 0x20 && t < 0x7F ? static_cast<char>(t) : ' ';
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) text += line + "\n";
    }
    return text;
}

void Snes::applyRamCheats() {
    for (const CheatPatch& p : ramCheats_) {
        const uint32_t bank = p.address >> 16;
        if ((bank & 0xFE) == 0x7E) wram_[p.address & 0x1FFFF] = static_cast<uint8_t>(p.value);
        else if (!(bank & 0x40) && (p.address & 0xFFFF) < 0x2000) wram_[p.address & 0x1FFF] = static_cast<uint8_t>(p.value);
        else cart_.write(p.address, static_cast<uint8_t>(p.value));
    }
}

void Snes::runFrame() {
    if (!loaded_) return;
    applyRamCheats();
    frameDone_ = false;
    // A frame is about 357 000 master clocks; give up after two in case one never ends.
    const uint64_t limit = master_ + 800000;
    while (!frameDone_ && master_ < limit) cpu_.step();
    apu_.catchUp();
    if (copro_) copro_->catchUp();
    apu_.takeSamples(audio_.output());
    ++frames_;
    frame_->frameNumber = frames_;
}

bool Snes::hasSaveData() const {
    return (cart_.info().battery && !cart_.ram().empty()) || (copro_ && !copro_->batteryData().empty());
}

// The cartridge's RAM, then the chip's own memory if it keeps one.
std::vector<uint8_t> Snes::saveData() const {
    if (!hasSaveData()) return {};
    std::vector<uint8_t> out;
    if (cart_.info().battery) out = cart_.ram();
    if (copro_) {
        const std::vector<uint8_t> chip = copro_->batteryData();
        out.insert(out.end(), chip.begin(), chip.end());
    }
    return out;
}

void Snes::loadSaveData(const std::vector<uint8_t>& data) {
    size_t used = 0;
    if (cart_.info().battery) {
        std::vector<uint8_t>& ram = cart_.ram();
        used = std::min(data.size(), ram.size());
        std::copy_n(data.begin(), used, ram.begin());
    }
    if (copro_ && used < data.size())
        copro_->loadBatteryData(std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(used), data.end()));
}

const char* Snes::cheatFormats() { return N_("Game Genie (DD62-3B1D) o Pro Action Replay (7E0DBF09)"); }

CheatDecoder Snes::decoder() {
    return [](const std::string& text, CheatPatch* out, std::string* error) {
        std::string c;
        for (char ch : text)
            if (ch != ' ') c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        CheatPatch p;
        if (c.size() == 9 && c[4] == '-') {
            // Game Genie: its digits, then the address's bits back in order.
            uint32_t v = 0;
            for (size_t i = 0; i < 9; ++i) {
                if (i == 4) continue;
                const char* d = std::strchr(kGenieDigits, c[i]);
                if (!c[i] || !d) {
                    if (error) *error = tr("un código de Game Genie de Super Nintendo es como DD62-3B1D");
                    return false;
                }
                v = v << 4 | static_cast<uint32_t>(d - kGenieDigits);
            }
            const uint32_t t = v & 0xFFFFFF;
            p.value = static_cast<uint16_t>(v >> 24);
            p.address = ((t & 0x003C00) << 10) | ((t & 0x00003C) << 14) | ((t & 0xF00000) >> 8) | ((t & 0x000003) << 10) |
                        ((t & 0x00C000) >> 6) | ((t & 0x0F0000) >> 12) | ((t & 0x0003C0) >> 6);
        } else {
            // Pro Action Replay: the address and the byte, AAAAAAVV or AAAAAA:VV.
            std::string hex = c;
            const size_t colon = hex.find(':');
            if (colon != std::string::npos) hex.erase(colon, 1);
            const bool ok = (colon == std::string::npos || colon == 6) && hex.size() == 8 &&
                            std::all_of(hex.begin(), hex.end(), [](char ch) { return hexDigit(ch) >= 0; });
            if (!ok) {
                if (error)
                    *error = tr("un código de Super Nintendo es un Game Genie (DD62-3B1D) o un Pro Action Replay (7E0DBF09)");
                return false;
            }
            uint32_t v = 0;
            for (char ch : hex) v = v << 4 | static_cast<uint32_t>(hexDigit(ch));
            p.address = v >> 8;
            p.value = static_cast<uint16_t>(v & 0xFF);
            const uint32_t bank = p.address >> 16, a = p.address & 0xFFFF;
            const bool wram = (bank & 0xFE) == 0x7E || (!(bank & 0x40) && a < 0x2000);
            const bool sram = ((bank & 0x7F) >= 0x70 && (bank & 0x7F) < 0x7E && a < 0x8000) ||
                              ((bank & 0x7F) >= 0x20 && (bank & 0x7F) < 0x40 && a >= 0x6000 && a < 0x8000);
            p.ram = wram || sram;
        }
        if (out) *out = p;
        return true;
    };
}

void Snes::setCheats(const std::vector<CheatPatch>& patches) {
    romCheats_.clear();
    ramCheats_.clear();
    for (const CheatPatch& p : patches) (p.ram ? ramCheats_ : romCheats_).push_back(p);
}

std::vector<MemoryRegion> Snes::memoryMap() {
    // RetroAchievements' map of the SNES: the 128 KB of WRAM, then the cartridge's RAM.
    std::vector<MemoryRegion> m;
    m.push_back({0x000000, wram_.data(), wram_.size()});
    if (!cart_.ram().empty()) m.push_back({0x020000, cart_.ram().data(), cart_.ram().size()});
    return m;
}

void Snes::saveBlocks(StateBlocks& blocks) {
    blocks.add("SNCP", [&](StateWriter& w) { cpu_.state(w); });
    blocks.add("SNMM", [&](StateWriter& w) {
        w(wram_, wmadd_, mdr_, pal_, master_, hc_, line_, refreshed_, hdmaDoneLine_, irqDoneLine_);
        w(nmitimen_, wrio_, wrmpya_, wrmpyb_, memsel_, hdmaen_, wrdiv_, rddiv_, rdmpy_, htime_, vtime_, rdnmi_, timeup_,
          autoJoyBusyUntil_, joy_, joyStrobe_, joyShift_);
    });
    blocks.add("SNDM", [&](StateWriter& w) {
        for (Channel& c : dma_)
            w(c.dmap, c.bbad, c.a1b, c.dasb, c.ntrl, c.unused, c.a1t, c.das, c.a2a, c.hdmaDone, c.hdmaDo);
    });
    blocks.add("SNPP", [&](StateWriter& w) { ppu_.state(w); });
    blocks.add("SNAP", [&](StateWriter& w) { apu_.state(w); });
    blocks.add("SNCT", [&](StateWriter& w) { w(cart_.ram()); });
    if (copro_) blocks.add("SNCO", [&](StateWriter& w) { copro_->save(w); });
}

bool Snes::loadBlock(uint32_t id, StateReader& r) {
    if (id == fourcc("SNCP")) {
        cpu_.state(r);
    } else if (id == fourcc("SNMM")) {
        r(wram_, wmadd_, mdr_, pal_, master_, hc_, line_, refreshed_, hdmaDoneLine_, irqDoneLine_);
        r(nmitimen_, wrio_, wrmpya_, wrmpyb_, memsel_, hdmaen_, wrdiv_, rddiv_, rdmpy_, htime_, vtime_, rdnmi_, timeup_,
          autoJoyBusyUntil_, joy_, joyStrobe_, joyShift_);
        apu_.setMasterRate(pal_ ? 21281370.0 : 21477272.0);
    } else if (id == fourcc("SNDM")) {
        for (Channel& c : dma_)
            r(c.dmap, c.bbad, c.a1b, c.dasb, c.ntrl, c.unused, c.a1t, c.das, c.a2a, c.hdmaDone, c.hdmaDo);
    } else if (id == fourcc("SNPP")) {
        ppu_.state(r);
    } else if (id == fourcc("SNAP")) {
        apu_.state(r);
    } else if (id == fourcc("SNCT")) {
        std::vector<uint8_t> ram;
        r(ram);
        if (ram.size() == cart_.ram().size()) cart_.ram() = ram;
    } else if (id == fourcc("SNCO")) {
        if (copro_) copro_->load(r);
    }
    return r.ok();
}

}  // namespace plumnin
