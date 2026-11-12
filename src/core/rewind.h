#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace plumnin {

// The last few seconds of play as quick savestates, one per frame, so the game can
// run backwards. When it is full the oldest snapshot goes; it never holds more than
// `maxFrames` snapshots or `maxBytes` bytes.
class RewindBuffer {
public:
    void setLimits(size_t maxFrames, size_t maxBytes);
    size_t maxFrames() const { return maxFrames_; }

    void push(std::vector<uint8_t> state);
    // Takes the newest snapshot out; false when there is none left.
    bool pop(std::vector<uint8_t>& state);
    void clear();

    size_t size() const { return states_.size(); }
    bool empty() const { return states_.empty(); }
    size_t bytes() const { return bytes_; }

private:
    void trim();

    std::deque<std::vector<uint8_t>> states_;
    size_t bytes_ = 0;
    size_t maxFrames_ = 0;
    size_t maxBytes_ = 0;
};

// Turns a speed (1 normal, 0.5 slow motion, 4 fast forward) into how many console
// frames to run on each tick of the display, which ticks at the console's own rate.
// Fractions carry over, so at 0.5 every other tick runs a frame and at 2.5 the ticks
// alternate between 2 and 3.
class FramePacer {
public:
    int framesForTick(double speed);
    void reset() { carry_ = 0; }

private:
    double carry_ = 0;
};

}  // namespace plumnin
