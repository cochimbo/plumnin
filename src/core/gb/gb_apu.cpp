#include "gb_apu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plumnin::gb {

namespace {

const uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0}};

// What reads of FF10-FF3F give in the bits that cannot be read.
const uint8_t kReadMask[0x30] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,  // NR10-NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,  // -, NR21-NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,  // NR30-NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,  // -, NR41-NR44
    0x00, 0x00, 0x70,              // NR50-NR52
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int noisePeriod(uint8_t nr43) {
    const int r = nr43 & 7;
    return (r ? r * 16 : 8) << (nr43 >> 4);
}

}  // namespace

void Apu::reset(bool color) {
    color_ = color;
    sq_[0] = sq_[1] = Square{};
    wave_ = Wave{};
    noise_ = Noise{};
    std::memset(regs_, 0, sizeof regs_);
    power_ = true;
    seqStep_ = 0;
    nr50_ = nr51_ = 0;
    // The wave RAM's contents at power on: the Color's pattern; the Game Boy's are random.
    static const uint8_t dmgWave[16] = {0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C,
                                        0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA};
    for (int i = 0; i < 16; ++i) waveRam_[i] = color ? (i & 1 ? 0xFF : 0x00) : dmgWave[i];
    // The registers the boot ROM leaves.
    static const std::pair<uint16_t, uint8_t> boot[] = {
        {0xFF10, 0x80}, {0xFF11, 0xBF}, {0xFF12, 0xF3}, {0xFF13, 0xFF}, {0xFF14, 0x3F}, {0xFF16, 0x3F},
        {0xFF17, 0x00}, {0xFF18, 0xFF}, {0xFF19, 0x3F}, {0xFF1A, 0x7F}, {0xFF1B, 0xFF}, {0xFF1C, 0x9F},
        {0xFF1D, 0xFF}, {0xFF1E, 0x3F}, {0xFF20, 0xFF}, {0xFF21, 0x00}, {0xFF22, 0x00}, {0xFF23, 0x3F},
        {0xFF24, 0x77}, {0xFF25, 0xF3}};
    for (const auto& [a, v] : boot) write(a, v);
    // The boot sound played on the first channel and has faded out.
    sq_[0].on = true;
    sq_[0].volume = 0;
    acc_[0] = acc_[1] = 0;
    accCycles_ = 0;
    sampleClock_ = 0;
    dcL_ = dcR_ = 0;
    samples_.clear();
}

uint16_t Apu::sweepCalc(Square& s) {
    const uint16_t delta = s.shadow >> s.sweepShift;
    uint16_t f;
    if (s.sweepDown) {
        f = static_cast<uint16_t>(s.shadow - delta);
        s.negUsed = true;
    } else {
        f = static_cast<uint16_t>(s.shadow + delta);
    }
    if (f > 2047) s.on = false;
    return f;
}

void Apu::envelope(uint8_t& volume, bool up, uint8_t period, uint8_t& timer) {
    if (!period) return;
    if (timer) --timer;
    if (timer) return;
    timer = period;
    if (up && volume < 15) ++volume;
    else if (!up && volume > 0) --volume;
}

void Apu::frameSequencerClock() {
    if (!power_) return;
    const uint8_t step = seqStep_;
    seqStep_ = (seqStep_ + 1) & 7;
    auto length = [](bool lengthOn, int& len, bool& on) {
        if (lengthOn && len > 0 && --len == 0) on = false;
    };
    if (!(step & 1)) {
        for (auto& s : sq_) length(s.lengthOn, s.length, s.on);
        length(wave_.lengthOn, wave_.length, wave_.on);
        length(noise_.lengthOn, noise_.length, noise_.on);
    }
    if (step == 2 || step == 6) {
        Square& s = sq_[0];
        if (s.sweepTimer) --s.sweepTimer;
        if (!s.sweepTimer) {
            s.sweepTimer = s.sweepPeriod ? s.sweepPeriod : 8;
            if (s.sweepOn && s.sweepPeriod) {
                const uint16_t f = sweepCalc(s);
                if (f <= 2047 && s.sweepShift) {
                    s.freq = s.shadow = f;
                    sweepCalc(s);
                }
            }
        }
    }
    if (step == 7) {
        for (auto& s : sq_)
            if (s.on) envelope(s.volume, s.envUp, s.envPeriod, s.envTimer);
        if (noise_.on) envelope(noise_.volume, noise_.envUp, noise_.envPeriod, noise_.envTimer);
    }
}

void Apu::trigger(int ch) {
    ++triggers_[ch];
    if (ch < 2) {
        Square& s = sq_[ch];
        s.on = s.dac;
        s.timer = (2048 - s.freq) * 4;
        s.volume = s.envInit;
        s.envTimer = s.envPeriod;
        if (ch == 0) {
            s.shadow = s.freq;
            s.sweepTimer = s.sweepPeriod ? s.sweepPeriod : 8;
            s.sweepOn = s.sweepPeriod || s.sweepShift;
            s.negUsed = false;
            if (s.sweepShift) sweepCalc(s);
        }
    } else if (ch == 2) {
        // The Game Boy's bug: retriggered just as it reads a sample, the channel
        // overwrites the start of the wave RAM with the bytes around the one it reads.
        if (!color_ && wave_.on && wave_.timer <= 2) {
            const int at = ((wave_.pos + 1) >> 1) & 15;
            if (at < 4) waveRam_[0] = waveRam_[at];
            else std::memmove(waveRam_, waveRam_ + (at & ~3), 4);
        }
        wave_.on = wave_.dac;
        wave_.timer = (2048 - wave_.freq) * 2 + 6;
        wave_.pos = 0;
        wave_.loaded = 0;
    } else {
        noise_.on = noise_.dac;
        noise_.timer = noisePeriod(noise_.nr43);
        noise_.lfsr = 0x7FFF;
        noise_.volume = noise_.envInit;
        noise_.envTimer = noise_.envPeriod;
    }
}

// NRx4's length enable, with the extra clock it gets when enabled in the half of the
// frame sequencer's period that does not clock lengths, and the trigger.
void Apu::lengthWrite(bool& lengthOn, int& length, bool& on, uint8_t value, int max) {
    const bool was = lengthOn;
    lengthOn = value & 0x40;
    const bool extra = seqStep_ & 1;
    if (extra && !was && lengthOn && length > 0 && --length == 0 && !(value & 0x80)) on = false;
    if ((value & 0x80) && length == 0) {
        length = max;
        if (lengthOn && extra) --length;
    }
}

// The Game Boy reaches the wave RAM while channel 3 plays only within 2 clocks of the
// channel reading a sample.
bool Apu::waveJustRead() const {
    if (!wave_.loaded) return false;
    const int ago = wave_.loaded - wave_.timer;
    return ago >= 0 && ago < 2;
}

uint8_t Apu::read(uint16_t address) const {
    if (address >= 0xFF30) {
        if (!wave_.on) return waveRam_[address - 0xFF30];
        // While it plays, the wave RAM answers with the byte the channel is reading; the
        // Game Boy's only in the moment it reads it.
        if (color_ || waveJustRead()) return waveRam_[wave_.pos >> 1];
        return 0xFF;
    }
    const int r = address - 0xFF10;
    if (address == 0xFF26) {
        return static_cast<uint8_t>((power_ ? 0x80 : 0) | 0x70 | (sq_[0].on ? 1 : 0) | (sq_[1].on ? 2 : 0) |
                                    (wave_.on ? 4 : 0) | (noise_.on ? 8 : 0));
    }
    return regs_[r] | kReadMask[r];
}

void Apu::write(uint16_t address, uint8_t value) {
    if (address >= 0xFF30) {
        if (!wave_.on) waveRam_[address - 0xFF30] = value;
        else if (color_ || waveJustRead()) waveRam_[wave_.pos >> 1] = value;
        return;
    }
    const int r = address - 0xFF10;
    if (address == 0xFF26) {
        const bool on = value & 0x80;
        if (power_ && !on) {
            // Switched off: every register clears, but the Game Boy keeps the lengths.
            for (uint16_t a = 0xFF10; a <= 0xFF25; ++a) write(a, 0);
            for (int i = 0; i < 2; ++i) {
                const int len = sq_[i].length;
                sq_[i] = Square{};
                if (!color_) sq_[i].length = len;
            }
            const int wl = wave_.length, nl = noise_.length;
            wave_ = Wave{};
            noise_ = Noise{};
            if (!color_) wave_.length = wl, noise_.length = nl;
            std::memset(regs_, 0, 0x16);
            nr50_ = nr51_ = 0;
        } else if (!power_ && on) {
            seqStep_ = 0;
        }
        power_ = on;
        return;
    }
    if (!power_) {
        // Only the lengths can be written while it is off, and only on the Game Boy.
        if (color_) return;
        if (r == 0x01 || r == 0x06) sq_[r / 5].length = 64 - (value & 63);
        else if (r == 0x0B) wave_.length = 256 - value;
        else if (r == 0x10) noise_.length = 64 - (value & 63);
        return;
    }
    regs_[r] = value;
    switch (r) {
        case 0x00: {
            Square& s = sq_[0];
            s.sweepPeriod = (value >> 4) & 7;
            const bool down = value & 8;
            if (s.sweepDown && !down && s.negUsed) s.on = false;
            s.sweepDown = down;
            s.sweepShift = value & 7;
            break;
        }
        case 0x01:
        case 0x06: {
            Square& s = sq_[r / 5];
            s.duty = value >> 6;
            s.length = 64 - (value & 63);
            break;
        }
        case 0x02:
        case 0x07: {
            Square& s = sq_[r / 5];
            s.nrx2 = value;
            s.envInit = value >> 4;
            s.envUp = value & 8;
            s.envPeriod = value & 7;
            s.dac = value & 0xF8;
            if (!s.dac) s.on = false;
            break;
        }
        case 0x03:
        case 0x08: {
            Square& s = sq_[r / 5];
            s.freq = static_cast<uint16_t>((s.freq & 0x700) | value);
            break;
        }
        case 0x04:
        case 0x09: {
            const int ch = r / 5;
            Square& s = sq_[ch];
            s.freq = static_cast<uint16_t>((s.freq & 0xFF) | (value & 7) << 8);
            lengthWrite(s.lengthOn, s.length, s.on, value, 64);
            if (value & 0x80) trigger(ch);
            break;
        }
        case 0x0A:
            wave_.dac = value & 0x80;
            if (!wave_.dac) wave_.on = false;
            break;
        case 0x0B: wave_.length = 256 - value; break;
        case 0x0C: wave_.level = (value >> 5) & 3; break;
        case 0x0D: wave_.freq = static_cast<uint16_t>((wave_.freq & 0x700) | value); break;
        case 0x0E:
            wave_.freq = static_cast<uint16_t>((wave_.freq & 0xFF) | (value & 7) << 8);
            lengthWrite(wave_.lengthOn, wave_.length, wave_.on, value, 256);
            if (value & 0x80) trigger(2);
            break;
        case 0x10: noise_.length = 64 - (value & 63); break;
        case 0x11:
            noise_.nr42 = value;
            noise_.envInit = value >> 4;
            noise_.envUp = value & 8;
            noise_.envPeriod = value & 7;
            noise_.dac = value & 0xF8;
            if (!noise_.dac) noise_.on = false;
            break;
        case 0x12: noise_.nr43 = value; break;
        case 0x13:
            lengthWrite(noise_.lengthOn, noise_.length, noise_.on, value, 64);
            if (value & 0x80) trigger(3);
            break;
        case 0x14: nr50_ = value; break;
        case 0x15: nr51_ = value; break;
        default: break;
    }
}

void Apu::musicVoice(int ch, SynthMusic::VoiceState& out) const {
    using Kind = SynthMusic::Kind;
    out = {};
    out.triggers = triggers_[ch];
    if (!power_) return;
    const bool left = nr51_ & (0x10 << ch), right = nr51_ & (0x01 << ch);
    if (!left && !right) return;
    out.pan = left == right ? 64 : left ? 16 : 111;
    if (ch < 2) {
        const Square& s = sq_[ch];
        out.sounding = s.on && s.dac;
        out.note = SynthMusic::noteOf(131072.0 / (2048 - s.freq));
        out.amplitude = s.volume / 15.0;
        out.duty = s.duty;
        // The instrument: its duty and how its envelope goes, not its starting volume.
        const uint8_t key[2] = {s.duty, static_cast<uint8_t>(s.nrx2 & 0x0F)};
        out.fingerprint = SynthMusic::fingerprint(Kind::Pulse, SynthMusic::hash(key, 2));
    } else if (ch == 2) {
        static const double kLevel[4] = {0, 1, 0.5, 0.25};
        out.sounding = wave_.on && wave_.dac;
        out.note = SynthMusic::noteOf(65536.0 / (2048 - wave_.freq));
        out.amplitude = kLevel[wave_.level];
        out.fingerprint = SynthMusic::fingerprint(Kind::Wave, SynthMusic::hash(waveRam_, 16));
        static thread_local uint8_t samples[32];
        for (int i = 0; i < 16; ++i) {
            samples[i * 2] = waveRam_[i] >> 4;
            samples[i * 2 + 1] = waveRam_[i] & 15;
        }
        out.wave = samples;
    } else {
        out.sounding = noise_.on && noise_.dac;
        out.amplitude = noise_.volume / 15.0;
        // How high it is: 524288 / divisor / 2^(shift + 1) Hz, from 2 Hz to 1 MHz.
        const int r = noise_.nr43 & 7;
        const double hz = 524288.0 / (r ? r : 0.5) / (2 << (noise_.nr43 >> 4));
        out.noise = std::clamp(static_cast<int>((std::log2(hz) - 8) * 15 / 12), 0, 15);
        const uint8_t key[2] = {noise_.nr43, static_cast<uint8_t>(noise_.nr42 & 0x0F)};
        out.fingerprint = SynthMusic::fingerprint(Kind::Noise, SynthMusic::hash(key, 2));
    }
}

void Apu::run(int cycles) {
    for (auto& s : sq_) {
        if (!s.on) continue;
        s.timer -= cycles;
        while (s.timer <= 0) {
            s.timer += (2048 - s.freq) * 4;
            s.step = (s.step + 1) & 7;
        }
    }
    if (wave_.on) {
        wave_.timer -= cycles;
        while (wave_.timer <= 0) {
            wave_.loaded = (2048 - wave_.freq) * 2;
            wave_.timer += wave_.loaded;
            wave_.pos = (wave_.pos + 1) & 31;
            const uint8_t b = waveRam_[wave_.pos >> 1];
            wave_.sample = wave_.pos & 1 ? (b & 0x0F) : (b >> 4);
        }
    }
    if (noise_.on && (noise_.nr43 >> 4) < 14) {
        noise_.timer -= cycles;
        while (noise_.timer <= 0) {
            noise_.timer += noisePeriod(noise_.nr43);
            const uint16_t x = (noise_.lfsr ^ (noise_.lfsr >> 1)) & 1;
            noise_.lfsr = static_cast<uint16_t>((noise_.lfsr >> 1) | x << 14);
            if (noise_.nr43 & 8) noise_.lfsr = static_cast<uint16_t>((noise_.lfsr & ~0x40) | x << 6);
        }
    }
    mix(cycles);
}

void Apu::mix(int cycles) {
    int left = 0, right = 0;
    if (power_) {
        int out[4];
        for (int i = 0; i < 2; ++i) {
            const Square& s = sq_[i];
            const int d = s.on && kDuty[s.duty][s.step] ? s.volume : 0;
            out[i] = s.dac ? 2 * d - 15 : 0;
        }
        {
            static const int shift[4] = {4, 0, 1, 2};
            const int d = wave_.on ? wave_.sample >> shift[wave_.level] : 0;
            out[2] = wave_.dac ? 2 * d - 15 : 0;
        }
        {
            const int d = noise_.on && !(noise_.lfsr & 1) ? noise_.volume : 0;
            out[3] = noise_.dac ? 2 * d - 15 : 0;
        }
        for (int i = 0; i < 4; ++i) {
            if (gains_[i] != 100) out[i] = out[i] * gains_[i] / 100;
            if (nr51_ & (0x10 << i)) left += out[i];
            if (nr51_ & (0x01 << i)) right += out[i];
        }
        left *= ((nr50_ >> 4) & 7) + 1;
        right *= (nr50_ & 7) + 1;
    }
    acc_[0] += static_cast<int64_t>(left) * cycles;
    acc_[1] += static_cast<int64_t>(right) * cycles;
    accCycles_ += cycles;
    sampleClock_ += cycles;
    if (sampleClock_ < cyclesPerSample_) return;
    sampleClock_ -= cyclesPerSample_;
    // The average since the last sample, then a high-pass that takes the DACs' offset off
    // the way the console's capacitor does.
    const double l = static_cast<double>(acc_[0]) / static_cast<double>(accCycles_);
    const double r = static_cast<double>(acc_[1]) / static_cast<double>(accCycles_);
    acc_[0] = acc_[1] = 0;
    accCycles_ = 0;
    dcL_ += (l - dcL_) * 0.0015;
    dcR_ += (r - dcR_) * 0.0015;
    auto clip = [](double v) { return static_cast<int16_t>(std::clamp(v * 60.0, -32767.0, 32767.0)); };
    samples_.push_back(clip(l - dcL_));
    samples_.push_back(clip(r - dcR_));
}

}  // namespace plumnin::gb
