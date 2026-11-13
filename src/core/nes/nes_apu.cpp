#include "nes_apu.h"

#include <algorithm>

namespace plumnin::nes {

namespace {

constexpr uint8_t kLengths[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                  12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 1, 1}, {0, 0, 0, 0, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 0, 0}};
constexpr uint16_t kNoise[2][16] = {
    {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068},
    {4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778}};
constexpr uint16_t kDmc[2][16] = {
    {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54},
    {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50}};
// The frame counter's steps, in processor cycles: NTSC and PAL, four and five steps.
constexpr uint32_t kSteps[2][2][6] = {
    {{7457, 14913, 22371, 29828, 29829, 29830}, {7457, 14913, 22371, 29829, 37281, 37282}},
    {{8313, 16627, 24939, 33252, 33253, 33254}, {8313, 16627, 24939, 33253, 41565, 41566}}};
// What each step clocks: 1 the envelopes and the linear counter, 2 also the lengths and sweeps.
constexpr uint8_t kStepKind[6] = {1, 2, 1, 0, 2, 0};

}  // namespace

void Apu::Envelope::clock(bool loop) {
    if (start) {
        start = false;
        decay = 15;
        divider = static_cast<int8_t>(volume);
        return;
    }
    if (--divider < 0) {
        divider = static_cast<int8_t>(volume);
        if (decay) --decay;
        else if (loop) decay = 15;
    }
}

void Apu::Length::load(uint8_t index) {
    if (!enabled) return;
    reload = kLengths[index & 31];
    previous = length;
}

int Apu::Pulse::target(bool first) const {
    const int change = period >> sweepShift;
    if (sweepNegate) return period - change - (first ? 1 : 0);
    return period + change;
}

void Apu::power(bool pal) {
    pal_ = pal;
    for (int i = 0; i < 31; ++i) pulseTable_[i] = i ? static_cast<float>(95.52 / (8128.0 / i + 100.0)) : 0.f;
    for (int i = 0; i < 203; ++i) tndTable_[i] = i ? static_cast<float>(163.67 / (24329.0 / i + 100.0)) : 0.f;
    pulse_[0] = pulse_[1] = Pulse{};
    tri_ = Triangle{};
    noise_ = Noise{};
    dmc_ = Dmc{};
    fiveStep_ = false;
    last4017_ = 0;
    reset();
    acc_ = sampleClock_ = dc_ = 0;
    accCycles_ = 0;
    samples_.clear();
    updateRate();
}

void Apu::reset() {
    // Everything is silenced as by a write of 0 to 4015; the DMC keeps its level.
    for (Pulse& p : pulse_) p.enabled = false, p.length = 0;
    tri_.enabled = false, tri_.length = 0;
    noise_.enabled = false, noise_.length = 0;
    const uint8_t level = dmc_.output;
    dmc_ = Dmc{};
    dmc_.output = level;
    dmc_.period = static_cast<uint16_t>(kDmc[pal_][0] - 1);
    dmc_.timer = dmc_.period;
    noise_.period = static_cast<uint16_t>(kNoise[pal_][0] - 1);
    // The frame counter acts as if 4017 were written again with its last value (0 at
    // power) a few cycles before the first instruction.
    frameIrq_ = dmcIrq_ = false;
    frameCycle_ = 0;
    frameStep_ = 0;
    newMode_ = last4017_;
    irqInhibit_ = last4017_ & 0x40;
    writeDelay_ = 3;
    blockTick_ = 0;
    dmcRequest_ = dmcAbort_ = false;
}

void Apu::updateRate() {
    const double cpu = pal_ ? 1662607.0 : 1789773.0;
    cyclesPerSample_ = cpu / std::max(sampleRate_, 1000.0);
}

void Apu::quarterFrame() {
    pulse_[0].env.clock(pulse_[0].halt);
    pulse_[1].env.clock(pulse_[1].halt);
    noise_.env.clock(noise_.halt);
    if (tri_.linearReload) tri_.linear = tri_.linearValue;
    else if (tri_.linear) --tri_.linear;
    if (!tri_.halt) tri_.linearReload = false;
}

void Apu::sweep(Pulse& p, bool first) {
    if (p.sweepDivider == 0 || --p.sweepDivider == 0) {
        if (p.sweepOn && p.sweepShift && !p.muted(first)) p.period = static_cast<uint16_t>(p.target(first));
        p.sweepDivider = p.sweepPeriod;
    }
    if (p.sweepReload) {
        p.sweepDivider = p.sweepPeriod;
        p.sweepReload = false;
    }
}

void Apu::halfFrame() {
    pulse_[0].tick();
    pulse_[1].tick();
    tri_.tick();
    noise_.tick();
    sweep(pulse_[0], true);
    sweep(pulse_[1], false);
}

void Apu::frameCounter() {
    ++frameCycle_;
    if (frameCycle_ == kSteps[pal_][fiveStep_][frameStep_]) {
        // In four steps the IRQ is raised on the last three cycles.
        if (!fiveStep_ && frameStep_ >= 3 && !irqInhibit_) frameIrq_ = true;
        const uint8_t kind = kStepKind[frameStep_];
        if (kind && !blockTick_) {
            quarterFrame();
            if (kind == 2) halfFrame();
            blockTick_ = 2;
        }
        if (++frameStep_ == 6) frameStep_ = 0, frameCycle_ = 0;
    }
    if (newMode_ >= 0 && --writeDelay_ == 0) {
        fiveStep_ = newMode_ & 0x80;
        newMode_ = -1;
        frameStep_ = 0;
        frameCycle_ = 0;
        // Five steps clock everything at once.
        if (fiveStep_ && !blockTick_) {
            quarterFrame();
            halfFrame();
            blockTick_ = 2;
        }
    }
    if (blockTick_) --blockTick_;
}

void Apu::dmcStart() {
    if (dmc_.empty && dmc_.remaining) dmcRequest_ = true;
}

void Apu::dmcFill(uint8_t value) {
    if (dmc_.remaining) {
        dmc_.buffer = value;
        dmc_.empty = false;
        dmc_.address = static_cast<uint16_t>(dmc_.address == 0xFFFF ? 0x8000 : dmc_.address + 1);
        if (--dmc_.remaining == 0) {
            if (dmc_.loop) {
                dmc_.address = dmc_.sampleAddress;
                dmc_.remaining = dmc_.sampleLength;
            } else if (dmc_.irqOn) {
                dmcIrq_ = true;
            }
        }
    }
}

void Apu::clock(uint64_t) {
    if (dmc_.disableDelay && --dmc_.disableDelay == 0) {
        dmc_.remaining = 0;
        dmcAbort_ = true;
    }
    if (dmc_.startDelay && --dmc_.startDelay == 0) dmcStart();

    frameCounter();
    pulse_[0].settle();
    pulse_[1].settle();
    tri_.settle();
    noise_.settle();

    for (Pulse& p : pulse_) {
        if (p.timer) {
            --p.timer;
        } else {
            p.timer = static_cast<uint16_t>(p.period * 2 + 1);
            p.step = (p.step - 1) & 7;
        }
    }
    if (tri_.timer) {
        --tri_.timer;
    } else {
        tri_.timer = tri_.period;
        if (tri_.length && tri_.linear) tri_.step = (tri_.step + 1) & 31;
    }
    if (noise_.timer) {
        --noise_.timer;
    } else {
        noise_.timer = noise_.period;
        const int feedback = (noise_.lfsr ^ (noise_.lfsr >> (noise_.mode ? 6 : 1))) & 1;
        noise_.lfsr = static_cast<uint16_t>(noise_.lfsr >> 1 | feedback << 14);
    }
    if (dmc_.timer) {
        --dmc_.timer;
    } else {
        dmc_.timer = dmc_.period;
        if (!dmc_.silent) {
            if (dmc_.shift & 1) {
                if (dmc_.output <= 125) dmc_.output += 2;
            } else if (dmc_.output >= 2) {
                dmc_.output -= 2;
            }
        }
        dmc_.shift >>= 1;
        if (--dmc_.bits == 0) {
            dmc_.bits = 8;
            if (dmc_.empty) {
                dmc_.silent = true;
            } else {
                dmc_.silent = false;
                dmc_.shift = dmc_.buffer;
                dmc_.empty = true;
                dmcStart();
            }
        }
    }
    mix();
}

void Apu::mix() {
    int p = 0;
    for (int i = 0; i < 2; ++i) {
        const Pulse& s = pulse_[i];
        if (s.length && !s.muted(i == 0) && kDuty[s.duty][s.step]) p += s.env.output() * gains_[i] / 100;
    }
    int t = tri_.step < 16 ? 15 - tri_.step : tri_.step - 16;
    int n = noise_.length && !(noise_.lfsr & 1) ? noise_.env.output() : 0;
    if (gains_[2] != 100) t = t * gains_[2] / 100;
    if (gains_[3] != 100) n = n * gains_[3] / 100;
    acc_ += pulseTable_[p] + tndTable_[3 * t + 2 * n + dmc_.output];
    ++accCycles_;
    sampleClock_ += 1;
    if (sampleClock_ < cyclesPerSample_) return;
    sampleClock_ -= cyclesPerSample_;
    const double v = acc_ / accCycles_;
    acc_ = 0;
    accCycles_ = 0;
    dc_ += (v - dc_) * 0.002;
    const int s = std::clamp(static_cast<int>((v - dc_) * 40000.0), -32768, 32767);
    samples_.push_back(static_cast<int16_t>(s));
    samples_.push_back(static_cast<int16_t>(s));
}

void Apu::musicVoice(int ch, double cpuHz, SynthMusic::VoiceState& out) const {
    using Kind = SynthMusic::Kind;
    out = {};
    out.triggers = triggers_[ch];
    if (ch < 2) {
        const Pulse& p = pulse_[ch];
        out.sounding = p.length && !p.muted(ch == 0);
        out.note = SynthMusic::noteOf(cpuHz / (16.0 * (p.period + 1)));
        out.amplitude = p.env.output() / 15.0;
        out.duty = p.duty;
        out.pan = ch ? 80 : 48;  // a little apart, as the two pulses often play together
        // The instrument: its duty and its envelope (or a volume set by the game).
        const uint8_t key[3] = {p.duty, static_cast<uint8_t>(p.env.constant), p.env.constant ? uint8_t{0} : p.env.volume};
        out.fingerprint = SynthMusic::fingerprint(Kind::Pulse, SynthMusic::hash(key, 3));
    } else if (ch == 2) {
        // Games silence it with the counters, or with a period too high to hear.
        out.sounding = tri_.length && tri_.linear && tri_.period >= 2;
        out.note = SynthMusic::noteOf(cpuHz / (32.0 * (tri_.period + 1)));
        out.amplitude = 1;
        out.fingerprint = SynthMusic::fingerprint(Kind::Triangle, 1);
    } else {
        out.sounding = noise_.length != 0;
        out.amplitude = noise_.env.output() / 15.0;
        out.noise = 15 - noiseIndex_;
        const uint8_t key[2] = {noiseIndex_, static_cast<uint8_t>(noise_.mode)};
        out.fingerprint = SynthMusic::fingerprint(Kind::Noise, SynthMusic::hash(key, 2));
    }
}

uint8_t Apu::peekStatus() const {
    uint8_t v = 0;
    if (pulse_[0].length) v |= 1;
    if (pulse_[1].length) v |= 2;
    if (tri_.length) v |= 4;
    if (noise_.length) v |= 8;
    if (dmc_.remaining) v |= 0x10;
    if (frameIrq_) v |= 0x40;
    if (dmcIrq_) v |= 0x80;
    return v;
}

uint8_t Apu::readStatus(uint64_t) {
    const uint8_t v = peekStatus();
    frameIrq_ = false;
    return v;
}

void Apu::write(uint16_t address, uint8_t v, uint64_t cycle) {
    switch (address) {
        case 0x4000:
        case 0x4004: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.duty = v >> 6;
            p.newHalt = v & 0x20;
            p.env.constant = v & 0x10;
            p.env.volume = v & 0x0F;
            break;
        }
        case 0x4001:
        case 0x4005: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.sweepOn = v & 0x80;
            p.sweepPeriod = static_cast<uint8_t>(((v >> 4) & 7) + 1);
            p.sweepNegate = v & 0x08;
            p.sweepShift = v & 7;
            p.sweepReload = true;
            break;
        }
        case 0x4002:
        case 0x4006: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.period = static_cast<uint16_t>((p.period & 0x700) | v);
            break;
        }
        case 0x4003:
        case 0x4007: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.period = static_cast<uint16_t>((p.period & 0xFF) | (v & 7) << 8);
            p.load(v >> 3);
            ++triggers_[(address >> 2) & 1];
            p.step = 0;
            p.env.start = true;
            break;
        }
        case 0x4008:
            tri_.newHalt = v & 0x80;
            tri_.linearValue = v & 0x7F;
            break;
        case 0x400A: tri_.period = static_cast<uint16_t>((tri_.period & 0x700) | v); break;
        case 0x400B:
            tri_.period = static_cast<uint16_t>((tri_.period & 0xFF) | (v & 7) << 8);
            tri_.load(v >> 3);
            tri_.linearReload = true;
            ++triggers_[2];
            break;
        case 0x400C:
            noise_.newHalt = v & 0x20;
            noise_.env.constant = v & 0x10;
            noise_.env.volume = v & 0x0F;
            break;
        case 0x400E:
            noise_.mode = v & 0x80;
            noiseIndex_ = v & 0x0F;
            noise_.period = static_cast<uint16_t>(kNoise[pal_][v & 0x0F] - 1);
            break;
        case 0x400F:
            noise_.load(v >> 3);
            noise_.env.start = true;
            ++triggers_[3];
            break;
        case 0x4010:
            dmc_.irqOn = v & 0x80;
            dmc_.loop = v & 0x40;
            dmc_.period = static_cast<uint16_t>(kDmc[pal_][v & 0x0F] - 1);
            if (!dmc_.irqOn) dmcIrq_ = false;
            break;
        case 0x4011: dmc_.output = v & 0x7F; break;
        case 0x4012: dmc_.sampleAddress = static_cast<uint16_t>(0xC000 | v << 6); break;
        case 0x4013: dmc_.sampleLength = static_cast<uint16_t>(v << 4 | 1); break;
        case 0x4015: {
            Length* lengths[4] = {&pulse_[0], &pulse_[1], &tri_, &noise_};
            for (int i = 0; i < 4; ++i) {
                lengths[i]->enabled = v & (1 << i);
                if (!lengths[i]->enabled) lengths[i]->length = 0;
            }
            dmcIrq_ = false;
            if (!(v & 0x10)) {
                // Disabling takes an APU cycle; a fetch starting meanwhile is cancelled.
                if (!dmc_.disableDelay) dmc_.disableDelay = (cycle & 1) == 0 ? 2 : 3;
            } else if (!dmc_.remaining) {
                dmc_.address = dmc_.sampleAddress;
                dmc_.remaining = dmc_.sampleLength;
                dmc_.startDelay = (cycle & 1) == 0 ? 2 : 3;
            }
            break;
        }
        case 0x4017:
            newMode_ = v;
            last4017_ = v;
            writeDelay_ = (cycle & 1) ? 4 : 3;
            irqInhibit_ = v & 0x40;
            if (irqInhibit_) frameIrq_ = false;
            break;
        default: break;
    }
}

}  // namespace plumnin::nes
