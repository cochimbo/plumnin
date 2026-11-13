#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "../state_io.h"
#include "../synth_music.h"
#include "snes_smp.h"

namespace plumnin::snes {

// The S-DSP: eight voices that play BRR samples from the sound module's RAM, with
// their Gaussian interpolation, ADSR and GAIN envelopes, noise and pitch modulation,
// and the echo with its eight-tap filter. One stereo sample every 32 cycles of the
// SPC700, 32 000 a second.
class Dsp {
public:
    explicit Dsp(std::array<uint8_t, 0x10000>& ram);
    void power();
    uint8_t read(uint8_t address) const { return regs_[address & 0x7F]; }
    void write(uint8_t address, uint8_t value);
    // One sample, added to `out` as left and right.
    void sample(std::vector<int16_t>& out);

    // What voice `i` is playing, for the synthesizer: its sample (by a fingerprint of
    // its BRR data, with the note it was recorded at worked out once), its pitch, its
    // volume and its key ons. A voice playing the noise is Kind::Noise.
    void musicVoice(int i, SynthMusic::VoiceState& out);
    // The voices (a bit each) heard at full volume; the rest at `percent`.
    void setVoiceGains(uint32_t full, int percent) {
        for (int i = 0; i < 8; ++i) gains_[static_cast<size_t>(i)] = (full >> i) & 1 ? 100 : percent;
    }
    // A sample in the sound RAM, decoded from its BRR blocks (up to 2 seconds): where it
    // loops back to and whether it does. Public for the tests.
    static std::vector<int16_t> decodeSample(const std::array<uint8_t, 0x10000>& ram, uint16_t start, uint16_t loop,
                                             size_t* loopIndex, bool* looped);

    template <class Ar>
    void state(Ar& ar) {
        ar(regs_, counter_, noise_, everyOther_, newKon_, kon_, koff_, echoOffset_, echoLength_, esa_, echoPos_);
        for (auto& h : echoHistory_) ar(h);
        for (Voice& v : voices_) {
            ar(v.buffer, v.bufferPos, v.interpPos, v.brrAddr, v.brrOffset, v.konDelay, v.envMode, v.env, v.hiddenEnv);
        }
    }

private:
    enum EnvMode : uint8_t { Release, Attack, Decay, Sustain };
    struct Voice {
        std::array<int16_t, 12> buffer{};
        uint8_t bufferPos = 0;
        int32_t interpPos = 0;
        uint16_t brrAddr = 0;
        uint8_t brrOffset = 1;
        uint8_t konDelay = 0;
        uint8_t envMode = Release;
        int32_t env = 0, hiddenEnv = 0;
    };

    bool counterFires(int rate) const;
    void runEnvelope(Voice& v, int index);
    void decodeBrr(Voice& v, uint8_t header);
    int interpolate(const Voice& v) const;
    uint8_t& reg(int voice, int r) { return regs_[static_cast<size_t>(voice * 0x10 + r)]; }

    std::array<uint8_t, 0x10000>& ram_;
    std::array<uint8_t, 128> regs_{};
    std::array<Voice, 8> voices_{};
    int32_t counter_ = 0;
    int32_t noise_ = 0x4000;
    bool everyOther_ = true;
    uint8_t newKon_ = 0, kon_ = 0, koff_ = 0;
    uint16_t echoOffset_ = 0, echoLength_ = 0;
    uint8_t esa_ = 0;
    uint8_t echoPos_ = 0;
    std::array<std::array<int32_t, 2>, 8> echoHistory_{};

    // For the synthesizer, not part of the state.
    struct MusicSample {
        uint32_t fingerprint = 0;
        double hz = 0;  // its note at 32 kHz, 0 for none
        SynthMusic::Traits traits;
    };
    std::map<uint64_t, MusicSample> musicSamples_;  // by start, loop and a hash of its data
    std::array<const MusicSample*, 8> voiceSample_{};
    std::array<uint32_t, 8> kons_{}, sampledAt_{};
    std::array<uint16_t, 8> voiceSource_{};
    std::array<int, 8> gains_{100, 100, 100, 100, 100, 100, 100, 100};
};

// The sound module: the SPC700 with its 64 KB of RAM, the IPL ROM that loads the
// game's program, its three timers, the four ports it talks to the main processor
// through, and the DSP. It runs behind the main processor and catches up whenever
// the main processor reads or writes a port.
class Apu : private SmpBus {
public:
    Apu();
    void power();
    void reset();
    // The master clock's rate (NTSC or PAL), to convert its time into the SPC700's.
    void setMasterRate(double hz) { masterHz_ = static_cast<int64_t>(hz); }
    // Time passed on the main processor's side, in master clocks.
    void addTime(uint32_t clocks) { budget_ += static_cast<int64_t>(clocks) * kSmpHz; }
    void catchUp();

    uint8_t readPort(int port) {
        catchUp();
        return out_[static_cast<size_t>(port & 3)];
    }
    void writePort(int port, uint8_t value) {
        catchUp();
        in_[static_cast<size_t>(port & 3)] = value;
    }
    // The sound made, at the frontend's rate.
    void setSampleRate(double hz) { outRate_ = hz; }
    void takeSamples(std::vector<int16_t>& out);

    // The synthesizer (not owned, null for none): polled 500 times a second, its clock
    // the DSP's samples.
    void setSynth(SynthMusic* synth) { synth_ = synth; }
    uint64_t musicClock() const { return dspSamples_; }
    void setVoiceGains(uint32_t full, int percent) { dsp_.setVoiceGains(full, percent); }

    std::array<uint8_t, 0x10000>& ram() { return ram_; }

    template <class Ar>
    void state(Ar& ar) {
        smp_.state(ar);
        dsp_.state(ar);
        ar(ram_, in_, out_, control_, dspAddr_, test_, budget_, dspClock_, resamplePos_, lastL_, lastR_);
        for (Timer& t : timers_) ar(t.target, t.stage, t.counter, t.output, t.enabled);
    }

private:
    static constexpr int64_t kSmpHz = 1024000;
    struct Timer {
        uint8_t target = 0;
        uint8_t stage = 0;
        uint8_t counter = 0;
        uint8_t output = 0;
        bool enabled = false;
    };

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void idle() override { cycle(); }
    void cycle();

    Smp smp_;
    std::array<uint8_t, 0x10000> ram_{};
    Dsp dsp_;
    std::array<uint8_t, 4> in_{}, out_{};
    uint8_t control_ = 0x80, dspAddr_ = 0, test_ = 0x0A;
    std::array<Timer, 3> timers_{};
    int64_t masterHz_ = 21477272;
    int64_t budget_ = 0;
    uint8_t dspClock_ = 0;

    SynthMusic* synth_ = nullptr;
    uint64_t dspSamples_ = 0;
    std::vector<int16_t> samples_;  // at 32 kHz
    double outRate_ = 44100, resamplePos_ = 0;
    int16_t lastL_ = 0, lastR_ = 0;
};

}  // namespace plumnin::snes
