#include "audio.h"

#include <algorithm>
#include <cstdio>

namespace plumnin {

namespace {

constexpr double kFramesPerSecond = 59.922743;
constexpr double kTargetVideoFrames = 3.0;  // ~50 ms of latency
constexpr double kMaxRateAdjust = 0.005;    // +-0.5%, too small to hear as pitch

}  // namespace

Audio::Audio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device_) {
        std::fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return;
    }
    freq_ = have.freq;
    target_ = static_cast<uint32_t>(freq_ / kFramesPerSecond * kTargetVideoFrames);
    SDL_PauseAudioDevice(device_, 0);
}

Audio::~Audio() {
    if (device_) SDL_CloseAudioDevice(device_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

uint32_t Audio::queuedFrames() const { return SDL_GetQueuedAudioSize(device_) / (2 * sizeof(int16_t)); }

double Audio::rate() const {
    if (!ok()) return freq_;
    const double error = (static_cast<double>(target_) - queuedFrames()) / target_;
    return freq_ * (1.0 + kMaxRateAdjust * std::clamp(error, -1.0, 1.0));
}

void Audio::push(const std::vector<int16_t>& samples, int volume, bool mute) {
    if (!ok() || samples.empty()) return;
    const uint32_t queued = queuedFrames();
    const uint32_t incoming = static_cast<uint32_t>(samples.size() / 2);
    // Way behind (a stall, a window drag): drop this frame rather than keep the lag.
    if (queued > target_ * 3) return;

    buffer_.clear();
    // Ran dry (start, unpause): pad with silence up to the target so it doesn't
    // run dry again on the next frame.
    if (queued < incoming / 2 && target_ > queued + incoming) buffer_.resize(2 * (target_ - queued - incoming), 0);
    const int gain = mute ? 0 : std::clamp(volume, 0, 100);
    for (int16_t s : samples) buffer_.push_back(static_cast<int16_t>(s * gain / 100));
    SDL_QueueAudio(device_, buffer_.data(), static_cast<Uint32>(buffer_.size() * sizeof(int16_t)));
}

}  // namespace plumnin
