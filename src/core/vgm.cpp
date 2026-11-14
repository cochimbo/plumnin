#include "vgm.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gb/gb_apu.h"
#include "nes/nes_apu.h"
#include "i18n.h"
#include "miniz.h"
#include "synth_music.h"

namespace plumnin {

namespace {

constexpr double kTickRate = 44100.0;
constexpr double kGbClock = 4194304.0;

uint32_t get32(const std::vector<uint8_t>& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return static_cast<uint32_t>(d[at]) | static_cast<uint32_t>(d[at + 1]) << 8 | static_cast<uint32_t>(d[at + 2]) << 16 |
           static_cast<uint32_t>(d[at + 3]) << 24;
}

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

void append32(std::vector<uint8_t>& d, uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// GD3 tags are UTF-16; plumnin's strings are UTF-8.
void appendUtf16(std::vector<uint8_t>& d, const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        ++i;
        for (; more > 0 && i < s.size(); --more, ++i) c = c << 6 | (static_cast<uint8_t>(s[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            const uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            for (uint32_t u : {hi, lo}) {
                d.push_back(static_cast<uint8_t>(u));
                d.push_back(static_cast<uint8_t>(u >> 8));
            }
        } else {
            d.push_back(static_cast<uint8_t>(c));
            d.push_back(static_cast<uint8_t>(c >> 8));
        }
    }
    d.push_back(0);
    d.push_back(0);
}

std::string readUtf16(const std::vector<uint8_t>& d, size_t& at, size_t end) {
    std::string out;
    while (at + 1 < end) {
        uint32_t c = d[at] | d[at + 1] << 8;
        at += 2;
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && at + 1 < end) {
            const uint32_t lo = d[at] | d[at + 1] << 8;
            at += 2;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | c >> 18);
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<uint8_t> gzip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    size_t size = 0;
    void* deflated = tdefl_compress_mem_to_heap(in.data(), in.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
    if (!deflated) return in;
    const uint8_t* p = static_cast<const uint8_t*>(deflated);
    out.insert(out.end(), p, p + size);
    mz_free(deflated);
    append32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, in.data(), in.size())));
    append32(out, static_cast<uint32_t>(in.size()));
    return out;
}

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    const uint8_t flags = in[3];
    size_t at = 10;
    if (flags & 4) at += 2 + static_cast<size_t>(in[at] | in[at + 1] << 8);
    for (int f : {8, 16})
        if (flags & f) {
            while (at < in.size() && in[at]) ++at;
            ++at;
        }
    if (flags & 2) at += 2;
    if (at >= in.size()) return false;
    size_t size = 0;
    void* p = tinfl_decompress_mem_to_heap(in.data() + at, in.size() - at, &size, 0);
    if (!p) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out->assign(b, b + size);
    mz_free(p);
    return true;
}

// How many bytes each command takes after its own, for the ones this skips.
size_t commandLength(uint8_t c) {
    if (c >= 0x30 && c <= 0x3F) return 1;
    if (c == 0x4F || c == 0x50) return 1;
    if (c >= 0x40 && c <= 0x4E) return 2;
    if (c >= 0x51 && c <= 0x5F) return 2;
    if (c >= 0xA0 && c <= 0xBF) return 2;
    if (c >= 0xC0 && c <= 0xDF) return 3;
    if (c >= 0xE0) return 4;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Recording

void VgmWriter::begin(const uint8_t* regs, uint64_t clock, double clockHz) {
    nes_ = false;
    data_.clear();
    samples_ = 0;
    start_ = clock;
    clockHz_ = clockHz;
    recording_ = true;
    // Power on first, then every register as it is (without the triggers, which would
    // start notes), the wave RAM, and the panning and volume last.
    auto put = [&](int reg, uint8_t v) {
        data_.push_back(0xB3);
        data_.push_back(static_cast<uint8_t>(reg));
        data_.push_back(v);
    };
    put(0x16, regs[0x16] & 0x80);
    if (!(regs[0x16] & 0x80)) return;  // switched off: nothing else holds
    for (int r = 0x20; r < 0x30; ++r) put(r, regs[r]);
    for (int r = 0x00; r < 0x16; ++r) {
        uint8_t v = regs[r];
        if (r == 0x04 || r == 0x09 || r == 0x0E || r == 0x13) v &= 0x7F;
        put(r, v);
    }
}

void VgmWriter::nesBegin(const uint8_t* regs, const uint8_t* memory, uint64_t clock, double clockHz) {
    data_.clear();
    samples_ = 0;
    start_ = clock;
    clockHz_ = clockHz;
    recording_ = true;
    nes_ = true;
    // The samples' memory first, as a data block, then the registers: the channels on,
    // the frame counter's mode, each register, and the channels on again (writing the
    // last of a channel's registers restarts it, which a channel that is off ignores).
    constexpr uint32_t kSize = 0x4000;
    data_.insert(data_.end(), {0x67, 0x66, 0xC2});
    append32(data_, kSize + 2);
    data_.push_back(0x00);
    data_.push_back(0xC0);
    data_.insert(data_.end(), memory, memory + kSize);
    auto put = [&](int reg, uint8_t v) {
        data_.push_back(0xB4);
        data_.push_back(static_cast<uint8_t>(reg));
        data_.push_back(v);
    };
    put(0x15, 0x00);
    put(0x17, regs[0x17]);
    for (int r = 0x00; r < 0x14; ++r) put(r, regs[r]);
    put(0x15, regs[0x15] & 0x0F);
}

void VgmWriter::nesWrite(int reg, uint8_t value, uint64_t clock) {
    if (!recording_ || !nes_ || reg < 0 || reg >= 0x18 || reg == 0x14 || reg == 0x16) return;
    wait(clock);
    data_.push_back(0xB4);
    data_.push_back(static_cast<uint8_t>(reg));
    data_.push_back(value);
}

void VgmWriter::wait(uint64_t clock) {
    const uint64_t target = clock > start_ ? static_cast<uint64_t>(static_cast<double>(clock - start_) * kTickRate / clockHz_) : 0;
    while (samples_ < target) {
        const uint64_t n = std::min<uint64_t>(target - samples_, 65535);
        if (n <= 16) {
            data_.push_back(static_cast<uint8_t>(0x70 + n - 1));
        } else {
            data_.push_back(0x61);
            data_.push_back(static_cast<uint8_t>(n));
            data_.push_back(static_cast<uint8_t>(n >> 8));
        }
        samples_ += n;
    }
}

void VgmWriter::gbWrite(int reg, uint8_t value, uint64_t clock) {
    if (!recording_ || nes_ || reg < 0 || reg >= 0x30) return;
    wait(clock);
    data_.push_back(0xB3);
    data_.push_back(static_cast<uint8_t>(reg));
    data_.push_back(value);
}

VgmWriter::Mark VgmWriter::mark(uint64_t clock) {
    wait(clock);
    return {data_.size(), samples_};
}

std::vector<uint8_t> VgmWriter::finish(const Tag& tag, const Mark* end, const Mark* loop) {
    if (end && end->bytes <= data_.size()) {
        data_.resize(end->bytes);
        samples_ = end->samples;
    }
    if (loop && (loop->bytes > data_.size() || loop->samples >= samples_)) loop = nullptr;
    recording_ = false;
    std::vector<uint8_t> file(0x100, 0);
    std::memcpy(file.data(), "Vgm ", 4);
    put32(file, 0x08, 0x161);
    put32(file, 0x18, static_cast<uint32_t>(samples_));
    put32(file, 0x24, 60);
    put32(file, 0x34, 0x100 - 0x34);
    put32(file, nes_ ? 0x84 : 0x80, nes_ ? static_cast<uint32_t>(std::lround(clockHz_)) : static_cast<uint32_t>(kGbClock));
    if (loop) {
        put32(file, 0x1C, static_cast<uint32_t>(0x100 + loop->bytes - 0x1C));
        put32(file, 0x20, static_cast<uint32_t>(samples_ - loop->samples));
    }
    file.insert(file.end(), data_.begin(), data_.end());
    file.push_back(0x66);
    put32(file, 0x14, static_cast<uint32_t>(file.size() - 0x14));
    std::vector<uint8_t> gd3;
    for (const std::string& s : {tag.title, std::string(), tag.game, std::string(), std::string(nes_ ? "Nintendo Entertainment System" : "Nintendo Game Boy"),
                                 std::string(), std::string(), std::string(), std::string(), std::string("plumnin"),
                                 tag.notes})
        appendUtf16(gd3, s);
    file.insert(file.end(), {'G', 'd', '3', ' '});
    append32(file, 0x100);
    append32(file, static_cast<uint32_t>(gd3.size()));
    file.insert(file.end(), gd3.begin(), gd3.end());
    put32(file, 0x04, static_cast<uint32_t>(file.size() - 4));
    return gzip(file);
}

// ---------------------------------------------------------------------------------
// Reading

bool VgmFile::parse(std::vector<uint8_t> bytes, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::vector<uint8_t> d;
    if (!gunzip(bytes, &d)) d = std::move(bytes);
    if (d.size() < 0x40 || std::memcmp(d.data(), "Vgm ", 4) != 0) return fail(tr("No es un archivo VGM."));
    const uint32_t version = get32(d, 0x08);
    gbClock = version >= 0x161 ? get32(d, 0x80) & 0x3FFFFFFF : 0;
    nesClock = version >= 0x161 && !gbClock ? get32(d, 0x84) & 0x3FFFFFFF : 0;
    if (!gbClock && !nesClock) return fail(tr("El VGM no es de una Game Boy ni de una NES: no tiene su chip de sonido."));
    start = version >= 0x150 && get32(d, 0x34) ? 0x34 + get32(d, 0x34) : 0x40;
    if (start >= d.size()) return fail(tr("El VGM está cortado."));
    samples = get32(d, 0x18);
    loop = get32(d, 0x1C) ? 0x1C + get32(d, 0x1C) : 0;
    loopSamples = loop ? get32(d, 0x20) : 0;
    if (loop >= d.size() || loopSamples > samples) loop = 0, loopSamples = 0;
    title.clear();
    game.clear();
    if (const uint32_t g = get32(d, 0x14); g && 0x14 + g + 12 <= d.size() && std::memcmp(&d[0x14 + g], "Gd3 ", 4) == 0) {
        size_t at = 0x14 + g + 12;
        const size_t end = std::min(d.size(), at + get32(d, 0x14 + g + 8));
        title = readUtf16(d, at, end);
        readUtf16(d, at, end);
        game = readUtf16(d, at, end);
    }
    data = std::move(d);
    return true;
}

// ---------------------------------------------------------------------------------
// Playing

VgmPlayer::VgmPlayer() : apu_(std::make_unique<gb::Apu>()), nesApu_(std::make_unique<nes::Apu>()) {}

VgmPlayer::~VgmPlayer() = default;

void VgmPlayer::load(VgmFile file) {
    file_ = std::move(file);
    restart();
}

void VgmPlayer::restart() {
    nesApu_->power(false);
    nesMemory_.assign(0x10000, 0);
    nesCycles_ = nesPoll_ = 0;
    apu_->reset(false);
    // The boot ROM's state is not the file's: everything silent until it writes.
    apu_->write(0xFF26, 0x00);
    at_ = file_.start;
    ticks_ = waitUntil_ = 0;
    cycles_ = 0;
    sequencer_ = 8192;
    finished_ = file_.data.empty();
    if (synth_) synth_->allNotesOff();
}

void VgmPlayer::setSynth(SynthMusic* synth, int chipPercent) {
    if (synth != synth_) {
        if (synth_) synth_->allNotesOff();
        if (synth) synth->allNotesOff();
    }
    synth_ = synth;
    chipPercent_ = std::clamp(chipPercent, 0, 100);
    if (!synth_) {
        apu_->setChannelGains(~0u, 0, 100);
        nesApu_->setChannelGains(~0u, 0, 100);
    }
}

void VgmPlayer::runNes(uint64_t cycles) {
    const double hz = file_.nesClock;
    for (uint64_t i = 0; i < cycles; ++i) {
        nesApu_->clock(nesCycles_);
        // The samples the delta modulation channel asks for, from what the file put in memory.
        if (nesApu_->takeDmcRequest()) nesApu_->dmcFill(nesMemory_[nesApu_->dmcAddress()]);
        nesApu_->takeDmcAbort();
        ++nesCycles_;
        if (synth_ && nesCycles_ - nesPoll_ >= 3728) {
            nesPoll_ = nesCycles_;
            for (int ch = 0; ch < 4; ++ch) {
                SynthMusic::VoiceState s;
                nesApu_->musicVoice(ch, hz, s);
                synth_->update(ch, s, nesCycles_);
            }
            nesApu_->setChannelGains(synth_->chipVoices(), synth_->silentVoices(), chipPercent_);
        }
    }
}

void VgmPlayer::runChip(uint64_t cycles) {
    while (cycles > 0) {
        const uint64_t step = std::min(cycles, sequencer_);
        // As the console runs it, a few cycles at a time: it makes a sample at most
        // each time.
        for (uint64_t done = 0; done < step; done += 4) apu_->run(static_cast<int>(std::min<uint64_t>(4, step - done)));
        cycles -= step;
        cycles_ += step;
        sequencer_ -= step;
        if (sequencer_ == 0) {
            sequencer_ = 8192;
            apu_->frameSequencerClock();
            if (synth_) {
                for (int ch = 0; ch < 4; ++ch) {
                    SynthMusic::VoiceState s;
                    apu_->musicVoice(ch, s);
                    synth_->update(ch, s, cycles_);
                }
                apu_->setChannelGains(synth_->chipVoices(), synth_->silentVoices(), chipPercent_);
            }
        }
    }
}

// Runs the commands up to 44100 Hz tick `tick`, the chip along with them.
void VgmPlayer::runTo(uint64_t tick) {
    const std::vector<uint8_t>& d = file_.data;
    auto chipTo = [&](uint64_t t) {
        if (file_.nesClock) {
            const uint64_t target = static_cast<uint64_t>(static_cast<double>(t) * file_.nesClock / kTickRate);
            if (target > nesCycles_) runNes(target - nesCycles_);
            return;
        }
        const uint64_t target = static_cast<uint64_t>(static_cast<double>(t) * kGbClock / kTickRate);
        if (target > cycles_) runChip(target - cycles_);
    };
    while (ticks_ < tick) {
        if (waitUntil_ > ticks_) {
            const uint64_t to = std::min(waitUntil_, tick);
            chipTo(to);
            ticks_ = to;
            continue;
        }
        if (finished_ || at_ >= d.size()) {
            finished_ = true;
            chipTo(tick);
            ticks_ = tick;
            return;
        }
        const uint8_t c = d[at_++];
        uint64_t wait = 0;
        if (c == 0x61 && at_ + 1 < d.size()) {
            wait = d[at_] | d[at_ + 1] << 8;
            at_ += 2;
        } else if (c == 0x62) {
            wait = 735;
        } else if (c == 0x63) {
            wait = 882;
        } else if (c >= 0x70 && c <= 0x7F) {
            wait = static_cast<uint64_t>(c - 0x70 + 1);
        } else if (c == 0x66) {
            if (file_.loop) at_ = file_.loop;
            else finished_ = true;
        } else if (c == 0x67 && at_ + 5 < d.size()) {  // a data block: the NES's samples, or skipped
            const uint8_t type = d[at_ + 1];
            const uint32_t size = get32(d, at_ + 2);
            const size_t body = at_ + 6;
            if (type == 0xC2 && size >= 2 && body + size <= d.size()) {
                const size_t address = d[body] | d[body + 1] << 8;
                for (size_t i = 2; i < size && address + i - 2 < nesMemory_.size(); ++i) nesMemory_[address + i - 2] = d[body + i];
            }
            at_ = body + size;
        } else if (c == 0xB3 && at_ + 1 < d.size()) {
            const uint8_t reg = d[at_], value = d[at_ + 1];
            at_ += 2;
            if (!(reg & 0x80)) apu_->write(static_cast<uint16_t>(0xFF10 + (reg & 0x7F)), value);
        } else if (c == 0xB4 && at_ + 1 < d.size()) {
            const uint8_t reg = d[at_], value = d[at_ + 1];
            at_ += 2;
            if (reg < 0x18) nesApu_->write(static_cast<uint16_t>(0x4000 + reg), value, nesCycles_);
        } else {
            at_ += commandLength(c);
        }
        if (wait) waitUntil_ = ticks_ + wait;
    }
}

void VgmPlayer::play(double seconds, double rate, std::vector<int16_t>& out) {
    const bool nes = file_.nesClock != 0;
    apu_->setSampleRate(rate);
    nesApu_->setSampleRate(rate);
    const size_t before = out.size() / 2;
    if (synth_) {
        synth_->setClock(nes ? file_.nesClock : kGbClock);
        synth_->beginFrame(nes ? nesCycles_ : cycles_);
    }
    const uint64_t from = ticks_;
    runTo(ticks_ + static_cast<uint64_t>(std::lround(seconds * kTickRate)));
    if (nes) nesApu_->takeSamples(out);
    else apu_->takeSamples(out);
    if (synth_) synth_->endFrame(nes ? nesCycles_ : cycles_, out, before, rate);
    // The chip's outputs jump from nothing to their resting level as it is switched on,
    // which the console's capacitor takes a moment to absorb: a fade in of the first
    // 50 ms instead of a click.
    constexpr double kFade = 0.05 * kTickRate;
    if (static_cast<double>(from) < kFade) {
        const size_t frames = out.size() / 2 - before;
        for (size_t i = 0; i < frames; ++i) {
            const double t = (static_cast<double>(from) + static_cast<double>(i) * kTickRate / rate) / kFade;
            if (t >= 1) break;
            for (int c = 0; c < 2; ++c) {
                int16_t& v = out[(before + i) * 2 + static_cast<size_t>(c)];
                v = static_cast<int16_t>(v * t);
            }
        }
    }
}

}  // namespace plumnin
