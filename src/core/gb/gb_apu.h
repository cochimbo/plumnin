#pragma once

#include <cstdint>
#include <vector>

#include "../state_io.h"
#include "../synth_music.h"

namespace plumnin::gb {

// The sound chip: two square waves (the first with a sweep), a wave of 32 samples
// and noise, mixed to stereo.
class Apu {
public:
    void reset(bool color);
    // `cycles` of the 4194304 Hz clock (2 per M-cycle in double speed, 4 otherwise).
    void run(int cycles);
    // A falling edge of the divider's bit 12 (13 in double speed): the frame sequencer.
    void frameSequencerClock();
    uint8_t read(uint16_t address) const;
    void write(uint16_t address, uint8_t value);

    void setSampleRate(double hz) { cyclesPerSample_ = 4194304.0 / hz; }
    // Moves what was made since the last call to `out`, interleaved stereo.
    void takeSamples(std::vector<int16_t>& out) {
        out.insert(out.end(), samples_.begin(), samples_.end());
        samples_.clear();
    }

    // What channel `ch` (0-3: the squares, the wave, the noise) is playing, for the
    // synthesizer.
    void musicVoice(int ch, SynthMusic::VoiceState& out) const;
    // The channels (a bit each) heard at full volume; the rest at `percent`.
    void setChannelGains(uint32_t full, int percent) {
        for (int i = 0; i < 4; ++i) gains_[i] = (full >> i) & 1 ? 100 : percent;
    }

    template <class Ar>
    void state(Ar& ar) {
        for (auto& s : sq_)
            ar(s.on, s.dac, s.length, s.lengthOn, s.duty, s.step, s.timer, s.freq, s.volume, s.envInit, s.envUp,
               s.envPeriod, s.envTimer, s.sweepPeriod, s.sweepDown, s.sweepShift, s.sweepTimer, s.sweepOn, s.shadow,
               s.negUsed, s.nrx2);
        ar(wave_.on, wave_.dac, wave_.length, wave_.lengthOn, wave_.timer, wave_.freq, wave_.pos, wave_.level,
           wave_.sample, wave_.loaded, waveRam_);
        ar(noise_.on, noise_.dac, noise_.length, noise_.lengthOn, noise_.timer, noise_.lfsr, noise_.nr43,
           noise_.volume, noise_.envInit, noise_.envUp, noise_.envPeriod, noise_.envTimer, noise_.nr42);
        ar(power_, nr50_, nr51_, seqStep_, regs_, acc_, accCycles_, sampleClock_, dcL_, dcR_);
    }

private:
    struct Square {
        bool on = false, dac = false;
        int length = 0;
        bool lengthOn = false;
        uint8_t duty = 0, step = 0;
        int timer = 0;
        uint16_t freq = 0;
        uint8_t volume = 0, envInit = 0;
        bool envUp = false;
        uint8_t envPeriod = 0, envTimer = 0;
        uint8_t sweepPeriod = 0;
        bool sweepDown = false;
        uint8_t sweepShift = 0, sweepTimer = 0;
        bool sweepOn = false;
        uint16_t shadow = 0;
        bool negUsed = false;
        uint8_t nrx2 = 0;
    };
    struct Wave {
        bool on = false, dac = false;
        int length = 0;
        bool lengthOn = false;
        int timer = 0;
        uint16_t freq = 0;
        uint8_t pos = 0, level = 0, sample = 0;
        int loaded = 0;  // the period the timer was last reloaded with (0: not since the trigger)
    };
    struct Noise {
        bool on = false, dac = false;
        int length = 0;
        bool lengthOn = false;
        int timer = 0;
        uint16_t lfsr = 0x7FFF;
        uint8_t nr43 = 0;
        uint8_t volume = 0, envInit = 0;
        bool envUp = false;
        uint8_t envPeriod = 0, envTimer = 0;
        uint8_t nr42 = 0;
    };

    uint16_t sweepCalc(Square& s);
    bool waveJustRead() const;
    void trigger(int ch);
    void lengthWrite(bool& lengthOn, int& length, bool& on, uint8_t value, int max);
    static void envelope(uint8_t& volume, bool up, uint8_t period, uint8_t& timer);
    void mix(int cycles);

    Square sq_[2];
    Wave wave_;
    Noise noise_;
    uint8_t waveRam_[16] = {};
    uint8_t regs_[0x30] = {};  // what was written, FF10-FF3F, for the reads
    bool power_ = true, color_ = false;
    uint8_t nr50_ = 0, nr51_ = 0;
    uint8_t seqStep_ = 0;

    double cyclesPerSample_ = 4194304.0 / 44100.0;
    double sampleClock_ = 0;
    int64_t acc_[2] = {};
    int64_t accCycles_ = 0;
    double dcL_ = 0, dcR_ = 0;
    std::vector<int16_t> samples_;
    uint32_t triggers_[4] = {};  // key ons, for the synthesizer
    int gains_[4] = {100, 100, 100, 100};
};

}  // namespace plumnin::gb
