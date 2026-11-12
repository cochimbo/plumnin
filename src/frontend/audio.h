#pragma once

#include <SDL.h>

#include <cstdint>
#include <vector>

namespace plumnin {

// SDL audio output for the console. The emulator is paced by the video loop, so instead
// of letting the two clocks drift apart (and crackle when the queue runs dry) it
// asks for a slightly faster or slower sample rate to keep the queue near a target.
class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const { return device_ != 0; }
    // Sample rate the console should produce at for the next frame.
    double rate() const;
    // The device's own sample rate, what rate() hovers around.
    int freq() const { return freq_; }
    // Queues interleaved stereo samples. volume is 0-100.
    void push(const std::vector<int16_t>& samples, int volume, bool mute);

private:
    uint32_t queuedFrames() const;

    SDL_AudioDeviceID device_ = 0;
    int freq_ = 44100;
    uint32_t target_ = 0;  // sample frames to keep queued
    std::vector<int16_t> buffer_;
};

}  // namespace plumnin
