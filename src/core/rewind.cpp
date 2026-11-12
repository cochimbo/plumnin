#include "rewind.h"

#include <algorithm>
#include <cmath>

namespace plumnin {

void RewindBuffer::setLimits(size_t maxFrames, size_t maxBytes) {
    maxFrames_ = maxFrames;
    maxBytes_ = maxBytes;
    trim();
}

void RewindBuffer::push(std::vector<uint8_t> state) {
    if (maxFrames_ == 0) return;
    bytes_ += state.size();
    states_.push_back(std::move(state));
    trim();
}

bool RewindBuffer::pop(std::vector<uint8_t>& state) {
    if (states_.empty()) return false;
    state = std::move(states_.back());
    states_.pop_back();
    bytes_ -= state.size();
    return true;
}

void RewindBuffer::clear() {
    states_.clear();
    bytes_ = 0;
}

void RewindBuffer::trim() {
    while (!states_.empty() && (states_.size() > maxFrames_ || bytes_ > maxBytes_)) {
        bytes_ -= states_.front().size();
        states_.pop_front();
    }
}

int FramePacer::framesForTick(double speed) {
    carry_ += std::max(0.0, speed);
    // A hair of slack so 0.1 * 10 still adds up to one frame.
    const double whole = std::floor(carry_ + 1e-9);
    carry_ = std::max(0.0, carry_ - whole);
    return static_cast<int>(whole);
}

}  // namespace plumnin
