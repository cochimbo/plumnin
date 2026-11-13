#pragma once

#include <cstdint>
#include <vector>

#include "../state_io.h"
#include "../synth_music.h"

namespace plumnin::nes {

// The 2A03's sound: two pulse waves, a triangle, noise and the delta modulation
// channel (DMC), which plays samples it fetches from memory with the processor's DMA.
// It runs one processor cycle at a time, as the frame counter that clocks the
// envelopes and lengths, and its IRQ, must land on the right cycle.
class Apu {
public:
    void power(bool pal);
    void reset();  // the reset button: everything silent, the frame counter's mode kept

    // One processor cycle; `cycle` is the processor's count, whose parity says on which
    // half of an APU cycle it falls.
    void clock(uint64_t cycle);
    uint8_t readStatus(uint64_t cycle);  // 4015
    uint8_t peekStatus() const;
    void write(uint16_t address, uint8_t value, uint64_t cycle);
    bool irq() const { return frameIrq_ || dmcIrq_; }

    // The DMC wants a byte: the processor halts to fetch it and hands it over.
    bool takeDmcRequest() {
        const bool r = dmcRequest_;
        dmcRequest_ = false;
        return r;
    }
    // A pending fetch was called off (the channel was disabled before it started).
    bool takeDmcAbort() {
        const bool r = dmcAbort_;
        dmcAbort_ = false;
        return r;
    }
    uint16_t dmcAddress() const { return dmc_.address; }
    void dmcFill(uint8_t value);

    void setSampleRate(double hz) { sampleRate_ = hz, updateRate(); }
    void takeSamples(std::vector<int16_t>& out) {
        out.insert(out.end(), samples_.begin(), samples_.end());
        samples_.clear();
    }

    // What channel `ch` (0-3: the pulses, the triangle, the noise) is playing, for the
    // synthesizer, with the processor's clock at `cpuHz`. The DMC's samples stay the console's.
    void musicVoice(int ch, double cpuHz, SynthMusic::VoiceState& out) const;
    // The channels (a bit each, as musicVoice's) heard at full volume; the rest at `percent`.
    void setChannelGains(uint32_t full, int percent) {
        for (int i = 0; i < 4; ++i) gains_[i] = (full >> i) & 1 ? 100 : percent;
    }

    template <class Ar>
    void state(Ar& ar) {
        for (Pulse& p : pulse_)
            ar(p.duty, p.step, p.timer, p.period, p.length, p.halt, p.newHalt, p.reload, p.previous, p.env.constant,
               p.env.volume, p.env.start, p.env.divider, p.env.decay, p.sweepOn, p.sweepPeriod, p.sweepNegate,
               p.sweepShift, p.sweepDivider, p.sweepReload, p.enabled);
        ar(tri_.timer, tri_.period, tri_.step, tri_.length, tri_.halt, tri_.newHalt, tri_.reload, tri_.previous,
           tri_.linear, tri_.linearReload, tri_.linearValue, tri_.enabled);
        ar(noise_.timer, noise_.period, noise_.mode, noise_.lfsr, noise_.length, noise_.halt, noise_.newHalt,
           noise_.reload, noise_.previous, noise_.env.constant, noise_.env.volume, noise_.env.start, noise_.env.divider,
           noise_.env.decay, noise_.enabled);
        ar(dmc_.timer, dmc_.period, dmc_.irqOn, dmc_.loop, dmc_.output, dmc_.sampleAddress, dmc_.sampleLength,
           dmc_.address, dmc_.remaining, dmc_.buffer, dmc_.empty, dmc_.shift, dmc_.bits, dmc_.silent,
           dmc_.startDelay, dmc_.disableDelay);
        ar(pal_, fiveStep_, irqInhibit_, frameIrq_, dmcIrq_, frameCycle_, frameStep_, newMode_, writeDelay_,
           blockTick_, last4017_, dmcRequest_, dmcAbort_, acc_, accCycles_, sampleClock_, dc_);
    }

private:
    struct Envelope {
        bool constant = false;
        uint8_t volume = 0;
        bool start = false;
        int8_t divider = 0;
        uint8_t decay = 0;
        void clock(bool loop);
        int output() const { return constant ? volume : decay; }
    };
    // What the length counters have in common: a load in the same cycle as a clock is
    // lost if the clock counted, and halt changes after that cycle's clock.
    struct Length {
        uint8_t length = 0;
        bool halt = false, newHalt = false;
        uint8_t reload = 0, previous = 0;
        bool enabled = false;
        void load(uint8_t index);
        void tick() {
            if (length && !halt) --length;
        }
        void settle() {
            if (reload) {
                if (length == previous) length = reload;
                reload = 0;
            }
            halt = newHalt;
        }
    };
    struct Pulse : Length {
        uint8_t duty = 0, step = 0;
        uint16_t timer = 0, period = 0;
        Envelope env;
        bool sweepOn = false, sweepNegate = false, sweepReload = false;
        uint8_t sweepPeriod = 1, sweepShift = 0, sweepDivider = 0;
        int target(bool first) const;
        bool muted(bool first) const { return period < 8 || (!sweepNegate && target(first) > 0x7FF); }
    };
    struct Triangle : Length {
        uint16_t timer = 0, period = 0;
        uint8_t step = 0;
        uint8_t linear = 0, linearValue = 0;
        bool linearReload = false;
    };
    struct Noise : Length {
        uint16_t timer = 0, period = 0;
        bool mode = false;
        uint16_t lfsr = 1;
        Envelope env;
    };
    struct Dmc {
        uint16_t timer = 0, period = 0;
        bool irqOn = false, loop = false;
        uint8_t output = 0;
        uint16_t sampleAddress = 0xC000, sampleLength = 1;
        uint16_t address = 0, remaining = 0;
        uint8_t buffer = 0;
        bool empty = true;
        uint8_t shift = 0, bits = 8;
        bool silent = true;
        uint8_t startDelay = 0, disableDelay = 0;
    };

    void quarterFrame();
    void halfFrame();
    void frameCounter();
    void sweep(Pulse& p, bool first);
    void dmcStart();
    void mix();
    void updateRate();

    Pulse pulse_[2];
    Triangle tri_;
    Noise noise_;
    Dmc dmc_;

    bool pal_ = false;
    bool fiveStep_ = false, irqInhibit_ = false, frameIrq_ = false, dmcIrq_ = false;
    uint32_t frameCycle_ = 0;
    uint8_t frameStep_ = 0;
    int16_t newMode_ = -1;  // a 4017 write waiting to take effect
    uint8_t writeDelay_ = 0, blockTick_ = 0;
    uint8_t last4017_ = 0;  // a reset writes it again
    bool dmcRequest_ = false, dmcAbort_ = false;

    // Down to the frontend's rate: the average of the cycles of each sample, then a
    // high-pass that takes the DC off.
    double sampleRate_ = 44100, cyclesPerSample_ = 40;
    double acc_ = 0, sampleClock_ = 0, dc_ = 0;
    uint32_t accCycles_ = 0;
    std::vector<int16_t> samples_;
    float pulseTable_[31] = {}, tndTable_[203] = {};
    uint32_t triggers_[4] = {};  // writes that start a note, for the synthesizer
    uint8_t noiseIndex_ = 0;     // 400E's period, 0-15
    int gains_[4] = {100, 100, 100, 100};
};

}  // namespace plumnin::nes
