#include "snes_apu.h"

#include <algorithm>
#include <cmath>

namespace plumnin::snes {

namespace {

// The IPL ROM, the 64 bytes at FFC0 that receive the game's sound program through the
// ports.
constexpr uint8_t kIpl[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

// The DSP's Gaussian interpolation table: 512 points of the curve it was made from,
// each group of four that a sample uses scaled to sum to 2048.
const std::array<int16_t, 512>& gaussTable() {
    static const std::array<int16_t, 512> table = [] {
        const double pi = 3.14159265358979323846;
        double curve[512];
        for (int n = 0; n < 512; ++n) {
            const double k = 0.5 + n;
            const double s = std::sin(pi * k * 1.280 / 1024);
            const double t = (std::cos(pi * k * 2.000 / 1023) - 1) * 0.50;
            const double u = (std::cos(pi * k * 4.000 / 1023) - 1) * 0.08;
            curve[511 - n] = s * (t + u + 1.0) / k;
        }
        std::array<int16_t, 512> out{};
        for (int phase = 0; phase < 128; ++phase) {
            const double sum = curve[phase] + curve[phase + 256] + curve[511 - phase] + curve[255 - phase];
            const double scale = 2048.0 / sum;
            for (int i : {phase, phase + 256, 511 - phase, 255 - phase})
                out[static_cast<size_t>(i)] = static_cast<int16_t>(curve[i] * scale + 0.5);
        }
        return out;
    }();
    return table;
}

// The envelope and noise rates: how many samples between steps, and where in the
// common counter each one fires.
constexpr int kCounterRange = 2048 * 5 * 3;
constexpr int kRates[32] = {kCounterRange + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256,
                            192, 160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
constexpr int kOffsets[32] = {1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
                              0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// The registers.
enum : uint8_t {
    VolL = 0x0, VolR = 0x1, PitchL = 0x2, PitchH = 0x3, Srcn = 0x4, Adsr1 = 0x5, Adsr2 = 0x6, Gain = 0x7, Envx = 0x8, Outx = 0x9
};
enum : uint8_t {
    Mvoll = 0x0C, Mvolr = 0x1C, Evoll = 0x2C, Evolr = 0x3C, Kon = 0x4C, Koff = 0x5C, Flg = 0x6C, Endx = 0x7C,
    Efb = 0x0D, Pmon = 0x2D, Non = 0x3D, Eon = 0x4D, Dir = 0x5D, Esa = 0x6D, Edl = 0x7D
};

}  // namespace

// --- DSP --------------------------------------------------------------------------------

Dsp::Dsp(std::array<uint8_t, 0x10000>& ram) : ram_(ram) {}

void Dsp::power() {
    regs_.fill(0);
    regs_[Flg] = 0xE0;
    voices_ = {};
    counter_ = 0;
    noise_ = 0x4000;
    everyOther_ = true;
    newKon_ = kon_ = koff_ = 0;
    echoOffset_ = echoLength_ = 0;
    esa_ = 0;
    echoPos_ = 0;
    for (auto& h : echoHistory_) h = {0, 0};
}

void Dsp::write(uint8_t address, uint8_t value) {
    if (address & 0x80) return;  // 80-FF only mirror the registers for reading
    regs_[address] = value;
    if (address == Kon) newKon_ = value;
    if (address == Endx) regs_[Endx] = 0;
}

bool Dsp::counterFires(int rate) const { return (counter_ + kOffsets[rate]) % kRates[rate] == 0; }

void Dsp::runEnvelope(Voice& v, int index) {
    int env = v.env;
    if (v.envMode == Release) {
        env -= 8;
        if (env < 0) env = 0;
        v.env = env;
        return;
    }
    const uint8_t adsr1 = reg(index, Adsr1);
    int rate;
    int data = reg(index, Adsr2);
    if (adsr1 & 0x80) {
        if (v.envMode >= Decay) {
            --env;
            env -= env >> 8;
            rate = data & 0x1F;
            if (v.envMode == Decay) rate = ((adsr1 >> 3) & 0x0E) + 0x10;
        } else {
            rate = (adsr1 & 0x0F) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        }
    } else {
        data = reg(index, Gain);
        const int mode = data >> 5;
        if (mode < 4) {
            env = data * 0x10;
            rate = 31;
        } else {
            rate = data & 0x1F;
            if (mode == 4) {
                env -= 0x20;
            } else if (mode < 6) {
                --env;
                env -= env >> 8;
            } else {
                env += 0x20;
                if (mode > 6 && static_cast<unsigned>(v.hiddenEnv) >= 0x600) env += 0x8 - 0x20;
            }
        }
    }
    // The sustain level ends the decay.
    if ((env >> 8) == (data >> 5) && v.envMode == Decay) v.envMode = Sustain;
    v.hiddenEnv = env;
    if (static_cast<unsigned>(env) > 0x7FF) {
        env = env < 0 ? 0 : 0x7FF;
        if (v.envMode == Attack) v.envMode = Decay;
    }
    if (counterFires(rate)) v.env = env;
}

void Dsp::decodeBrr(Voice& v, uint8_t header) {
    int nibbles = ram_[static_cast<uint16_t>(v.brrAddr + v.brrOffset)] << 8 | ram_[static_cast<uint16_t>(v.brrAddr + v.brrOffset + 1)];
    const int shift = header >> 4;
    const int filter = header & 0x0C;
    for (int i = 0; i < 4; ++i, nibbles <<= 4) {
        int s = static_cast<int16_t>(static_cast<uint16_t>(nibbles)) >> 12;
        s = (s << shift) >> 1;
        if (shift >= 0xD) s = s < 0 ? -0x800 : 0;
        const int p1 = v.buffer[(v.bufferPos + 11) % 12];
        const int p2 = v.buffer[(v.bufferPos + 10) % 12] >> 1;
        if (filter >= 8) {
            s += p1;
            s -= p2;
            if (filter == 8) {
                s += p2 >> 4;
                s += (p1 * -3) >> 6;
            } else {
                s += (p1 * -13) >> 7;
                s += (p2 * 3) >> 4;
            }
        } else if (filter) {
            s += p1 >> 1;
            s += (-p1) >> 5;
        }
        s = clamp16(s);
        v.buffer[v.bufferPos] = static_cast<int16_t>(static_cast<uint16_t>(s * 2));
        v.bufferPos = static_cast<uint8_t>((v.bufferPos + 1) % 12);
    }
}

int Dsp::interpolate(const Voice& v) const {
    const auto& gauss = gaussTable();
    const int offset = (v.interpPos >> 4) & 0xFF;
    const int16_t* fwd = gauss.data() + 255 - offset;
    const int16_t* rev = gauss.data() + offset;
    int at = (v.bufferPos + (v.interpPos >> 12)) % 12;
    auto next = [&] {
        const int s = v.buffer[static_cast<size_t>(at)];
        at = (at + 1) % 12;
        return s;
    };
    int out = (fwd[0] * next()) >> 11;
    out += (fwd[256] * next()) >> 11;
    out += (rev[256] * next()) >> 11;
    out = static_cast<int16_t>(out);
    out += (rev[0] * next()) >> 11;
    return clamp16(out) & ~1;
}

void Dsp::sample(std::vector<int16_t>& out) {
    if (--counter_ < 0) counter_ = kCounterRange - 1;
    everyOther_ = !everyOther_;
    if (everyOther_) {
        newKon_ &= static_cast<uint8_t>(~kon_);
        kon_ = newKon_;
        koff_ = regs_[Koff];
    }
    if (counterFires(regs_[Flg] & 0x1F)) {
        const int feedback = (noise_ << 13) ^ (noise_ << 14);
        noise_ = (feedback & 0x4000) ^ (noise_ >> 1);
    }

    int mainOut[2] = {0, 0}, echoOut[2] = {0, 0};
    int lastOutput = 0;
    uint8_t endx = regs_[Endx];
    const uint8_t pmon = regs_[Pmon] & 0xFE, non = regs_[Non], eon = regs_[Eon];
    for (int i = 0; i < 8; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        const uint8_t bit = static_cast<uint8_t>(1 << i);
        const uint16_t dirAddr = static_cast<uint16_t>(regs_[Dir] * 0x100 + reg(i, Srcn) * 4);
        const uint16_t entry = static_cast<uint16_t>(dirAddr + (v.konDelay ? 0 : 2));
        const uint16_t nextAddr = static_cast<uint16_t>(ram_[entry] | ram_[static_cast<uint16_t>(entry + 1)] << 8);
        int pitch = reg(i, PitchL) | (reg(i, PitchH) & 0x3F) << 8;
        uint8_t header = ram_[v.brrAddr];
        if (pmon & bit) pitch += ((lastOutput >> 5) * pitch) >> 10;
        if (v.konDelay) {
            if (v.konDelay == 5) {
                v.brrAddr = nextAddr;
                v.brrOffset = 1;
                v.bufferPos = 0;
                header = 0;
            }
            v.env = v.hiddenEnv = 0;
            v.interpPos = 0;
            if (--v.konDelay & 3) v.interpPos = 0x4000;
            pitch = 0;
        }
        int output = interpolate(v);
        if (non & bit) output = static_cast<int16_t>(static_cast<uint16_t>(noise_ * 2));
        output = ((output * v.env) >> 11) & ~1;
        const uint8_t envx = static_cast<uint8_t>(v.env >> 4);
        if ((regs_[Flg] & 0x80) || (header & 3) == 1) {
            v.envMode = Release;
            v.env = 0;
        }
        if (everyOther_) {
            if (koff_ & bit) v.envMode = Release;
            if (kon_ & bit) {
                v.konDelay = 5;
                v.envMode = Attack;
            }
        }
        if (!v.konDelay) runEnvelope(v, i);

        // The next four samples of BRR, and on to the next block (or the loop) when
        // this one is done.
        bool looped = false;
        if (v.interpPos >= 0x4000) {
            decodeBrr(v, header);
            v.brrOffset = static_cast<uint8_t>(v.brrOffset + 2);
            if (v.brrOffset >= 9) {
                v.brrAddr = static_cast<uint16_t>(v.brrAddr + 9);
                if (header & 1) {
                    v.brrAddr = nextAddr;
                    looped = true;
                }
                v.brrOffset = 1;
            }
        }
        v.interpPos = (v.interpPos & 0x3FFF) + pitch;
        if (v.interpPos > 0x7FFF) v.interpPos = 0x7FFF;

        for (int ch = 0; ch < 2; ++ch) {
            const int amp = (output * static_cast<int8_t>(reg(i, VolL + ch))) >> 7;
            mainOut[ch] = clamp16(mainOut[ch] + amp);
            if (eon & bit) echoOut[ch] = clamp16(echoOut[ch] + amp);
        }
        if (looped) endx |= bit;
        if (v.konDelay == 5) endx = static_cast<uint8_t>(endx & ~bit);  // keyed on just now
        reg(i, Outx) = static_cast<uint8_t>(output >> 8);
        reg(i, Envx) = envx;
        lastOutput = output;
    }
    regs_[Endx] = endx;

    // The echo: the oldest sample of the buffer through the filter, mixed in, and the
    // voices that echo written back with its feedback.
    echoPos_ = static_cast<uint8_t>((echoPos_ + 1) & 7);
    const uint16_t echoPtr = static_cast<uint16_t>(esa_ * 0x100 + echoOffset_);
    for (int ch = 0; ch < 2; ++ch) {
        const uint16_t at = static_cast<uint16_t>(echoPtr + ch * 2);
        const int s = static_cast<int16_t>(static_cast<uint16_t>(ram_[at] | ram_[static_cast<uint16_t>(at + 1)] << 8));
        echoHistory_[echoPos_][static_cast<size_t>(ch)] = s >> 1;
    }
    int echoIn[2];
    for (int ch = 0; ch < 2; ++ch) {
        auto tap = [&](int t) {
            // Tap 0 is the oldest of the eight, tap 7 the sample just read.
            const int s = echoHistory_[static_cast<size_t>((echoPos_ + 1 + t) & 7)][static_cast<size_t>(ch)];
            return (s * static_cast<int8_t>(regs_[static_cast<size_t>(t * 0x10 + 0x0F)])) >> 6;
        };
        int sum = 0;
        for (int t = 0; t < 7; ++t) sum += tap(t);
        sum = static_cast<int16_t>(sum);
        sum += static_cast<int16_t>(tap(7));
        echoIn[ch] = clamp16(sum) & ~1;
    }
    int result[2];
    for (int ch = 0; ch < 2; ++ch) {
        result[ch] = clamp16(static_cast<int16_t>((mainOut[ch] * static_cast<int8_t>(regs_[ch ? Mvolr : Mvoll])) >> 7) +
                             static_cast<int16_t>((echoIn[ch] * static_cast<int8_t>(regs_[ch ? Evolr : Evoll])) >> 7));
        echoOut[ch] = clamp16(echoOut[ch] + static_cast<int16_t>((echoIn[ch] * static_cast<int8_t>(regs_[Efb])) >> 7)) & ~1;
    }
    if (regs_[Flg] & 0x40) result[0] = result[1] = 0;
    out.push_back(static_cast<int16_t>(result[0]));
    out.push_back(static_cast<int16_t>(result[1]));

    if (!echoOffset_) echoLength_ = static_cast<uint16_t>((regs_[Edl] & 0x0F) * 0x800);
    if (!(regs_[Flg] & 0x20)) {
        for (int ch = 0; ch < 2; ++ch) {
            const uint16_t at = static_cast<uint16_t>(echoPtr + ch * 2);
            ram_[at] = static_cast<uint8_t>(echoOut[ch]);
            ram_[static_cast<uint16_t>(at + 1)] = static_cast<uint8_t>(echoOut[ch] >> 8);
        }
    }
    echoOffset_ = static_cast<uint16_t>(echoOffset_ + 4);
    if (echoOffset_ >= echoLength_) echoOffset_ = 0;
    esa_ = regs_[Esa];
}

// --- The sound module -------------------------------------------------------------------

Apu::Apu() : dsp_(ram_) { smp_.setBus(this); }

void Apu::power() {
    // The RAM is not cleared by the hardware; games do not rely on what it holds.
    ram_.fill(0);
    reset();
}

void Apu::reset() {
    smp_.power();
    dsp_.power();
    in_.fill(0);
    out_.fill(0);
    control_ = 0x80;
    dspAddr_ = 0;
    test_ = 0x0A;
    timers_ = {};
    budget_ = 0;
    dspClock_ = 0;
    samples_.clear();
    smp_.r.pc = static_cast<uint16_t>(kIpl[62] | kIpl[63] << 8);
}

void Apu::catchUp() {
    while (budget_ > 0) smp_.step();
}

void Apu::cycle() {
    budget_ -= masterHz_;
    // T0 and T1 count at 8 kHz, T2 at 64 kHz.
    for (int t = 0; t < 3; ++t) {
        Timer& tm = timers_[static_cast<size_t>(t)];
        if (++tm.stage < (t == 2 ? 16 : 128)) continue;
        tm.stage = 0;
        if (!tm.enabled) continue;
        if (++tm.counter == tm.target) {
            tm.counter = 0;
            tm.output = (tm.output + 1) & 0x0F;
        }
    }
    if (++dspClock_ == 32) {
        dspClock_ = 0;
        dsp_.sample(samples_);
    }
}

uint8_t Apu::read(uint16_t a) {
    cycle();
    if (a >= 0xF0 && a <= 0xFF) {
        switch (a) {
            case 0xF2: return dspAddr_;
            case 0xF3: return dsp_.read(dspAddr_);
            case 0xF4:
            case 0xF5:
            case 0xF6:
            case 0xF7: return in_[a - 0xF4u];
            case 0xF8:
            case 0xF9: return ram_[a];
            case 0xFD:
            case 0xFE:
            case 0xFF: {
                Timer& t = timers_[a - 0xFDu];
                const uint8_t v = t.output;
                t.output = 0;
                return v;
            }
            default: return 0;
        }
    }
    if (a >= 0xFFC0 && (control_ & 0x80)) return kIpl[a - 0xFFC0u];
    return ram_[a];
}

void Apu::write(uint16_t a, uint8_t v) {
    cycle();
    ram_[a] = v;  // the registers' writes reach the RAM under them too
    if (a < 0xF0 || a > 0xFF) return;
    switch (a) {
        case 0xF0: test_ = v; break;
        case 0xF1:
            for (int t = 0; t < 3; ++t) {
                Timer& tm = timers_[static_cast<size_t>(t)];
                const bool on = v & (1 << t);
                if (on && !tm.enabled) tm.counter = tm.output = 0;
                tm.enabled = on;
            }
            if (v & 0x10) in_[0] = in_[1] = 0;
            if (v & 0x20) in_[2] = in_[3] = 0;
            control_ = v;
            break;
        case 0xF2: dspAddr_ = v; break;
        case 0xF3: dsp_.write(dspAddr_, v); break;
        case 0xF4:
        case 0xF5:
        case 0xF6:
        case 0xF7: out_[a - 0xF4u] = v; break;
        case 0xFA:
        case 0xFB:
        case 0xFC: timers_[a - 0xFAu].target = v; break;
        default: break;
    }
}

void Apu::takeSamples(std::vector<int16_t>& out) {
    // From 32 kHz to the frontend's rate, between each pair of samples.
    const double step = 32000.0 / std::max(outRate_, 1000.0);
    const size_t n = samples_.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const int16_t l = samples_[i * 2], r = samples_[i * 2 + 1];
        while (resamplePos_ < 1.0) {
            out.push_back(static_cast<int16_t>(lastL_ + (l - lastL_) * resamplePos_));
            out.push_back(static_cast<int16_t>(lastR_ + (r - lastR_) * resamplePos_));
            resamplePos_ += step;
        }
        resamplePos_ -= 1.0;
        lastL_ = l;
        lastR_ = r;
    }
    samples_.clear();
}

}  // namespace plumnin::snes
