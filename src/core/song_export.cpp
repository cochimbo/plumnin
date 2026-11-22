#include "song_export.h"

#include <algorithm>
#include <cmath>

#include "synth_music.h"
#include "vgm.h"

extern "C" {
#include <layer3.h>
}

namespace plumnin {

double renderedSeconds(const VgmFile& file, const SongRender& how) {
    double seconds;
    if (file.spc) {
        seconds = (file.samples > 0 ? file.seconds() : how.spcSeconds) + how.fadeSeconds;
    } else if (file.loop && file.loopSamples > 0) {
        const double loop = file.loopSamples / 44100.0;
        const double intro = std::max(0.0, file.seconds() - loop);
        seconds = intro + loop * std::max(1, how.loops) + how.fadeSeconds;
    } else {
        seconds = file.seconds() + 1.5;  // what still rings when it ends
    }
    return std::clamp(seconds, 1.0, how.maxSeconds);
}

bool renderSong(const VgmFile& file, SynthMusic* synth, int chipPercent, const SongRender& how, std::vector<int16_t>* out,
                std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    const double total = renderedSeconds(file, how);
    const bool fades = file.spc || (file.loop && file.loopSamples > 0) || total >= how.maxSeconds;
    VgmPlayer player;
    player.load(file);
    player.setLoop(true);
    player.setSynth(synth, chipPercent);
    player.setSynthOutput(synth != nullptr);
    const size_t frames = static_cast<size_t>(std::llround(total * how.rate));
    out->clear();
    out->reserve(frames * 2);
    constexpr double kStep = 1.0 / 60.0;
    while (out->size() / 2 < frames) {
        if (cancel && *cancel) {
            player.setSynth(nullptr, 100);
            return false;
        }
        if (player.finished()) {
            out->resize(out->size() + 2 * static_cast<size_t>(std::lround(kStep * how.rate)), 0);
        } else {
            player.play(kStep, how.rate, *out);
        }
        if (progress) *progress = static_cast<float>(std::min(1.0, static_cast<double>(out->size() / 2) / frames));
    }
    player.setSynth(nullptr, 100);
    out->resize(frames * 2);
    if (fades) {
        const size_t fade = std::min(frames, static_cast<size_t>(how.fadeSeconds * how.rate));
        for (size_t i = 0; i < fade; ++i) {
            const double g = 0.5 + 0.5 * std::cos(3.14159265358979 * static_cast<double>(i + 1) / static_cast<double>(fade));
            const size_t at = (frames - fade + i) * 2;
            for (size_t c = 0; c < 2; ++c)
                (*out)[at + c] = static_cast<int16_t>(std::lround((*out)[at + c] * g));
        }
    }
    return true;
}

namespace {

// An ID3v2.3 text frame, in UTF-16 with its byte order mark.
void textFrame(std::vector<uint8_t>& tag, const char* id, const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> body = {1, 0xFF, 0xFE};
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<uint8_t>(text[i]);
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        ++i;
        for (; more > 0 && i < text.size(); --more, ++i) c = c << 6 | (static_cast<uint8_t>(text[i]) & 0x3F);
        auto unit = [&](uint32_t u) {
            body.push_back(static_cast<uint8_t>(u));
            body.push_back(static_cast<uint8_t>(u >> 8));
        };
        if (c >= 0x10000) {
            c -= 0x10000;
            unit(0xD800 + (c >> 10));
            unit(0xDC00 + (c & 0x3FF));
        } else {
            unit(c);
        }
    }
    tag.insert(tag.end(), id, id + 4);
    const uint32_t size = static_cast<uint32_t>(body.size());
    for (int s = 24; s >= 0; s -= 8) tag.push_back(static_cast<uint8_t>(size >> s));
    tag.push_back(0);
    tag.push_back(0);
    tag.insert(tag.end(), body.begin(), body.end());
}

std::vector<uint8_t> id3(const Mp3Tag& t) {
    std::vector<uint8_t> frames;
    textFrame(frames, "TIT2", t.title);
    textFrame(frames, "TALB", t.album);
    textFrame(frames, "TPE1", t.artist);
    if (t.track > 0) textFrame(frames, "TRCK", std::to_string(t.track));
    textFrame(frames, "TSSE", "plumnin");
    std::vector<uint8_t> out = {'I', 'D', '3', 3, 0, 0};
    const uint32_t size = static_cast<uint32_t>(frames.size());
    for (int s = 21; s >= 0; s -= 7) out.push_back(static_cast<uint8_t>((size >> s) & 0x7F));  // synchsafe
    out.insert(out.end(), frames.begin(), frames.end());
    return out;
}

}  // namespace

std::vector<uint8_t> encodeMp3(const std::vector<int16_t>& stereo, int rate, int kbps, const Mp3Tag& tag) {
    if (shine_check_config(rate, kbps) < 0) return {};
    shine_config_t config{};
    config.wave.channels = PCM_STEREO;
    config.wave.samplerate = rate;
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.mpeg.mode = STEREO;
    config.mpeg.bitr = kbps;
    shine_t encoder = shine_initialise(&config);
    if (!encoder) return {};
    std::vector<uint8_t> out = id3(tag);
    const size_t pass = static_cast<size_t>(shine_samples_per_pass(encoder));
    std::vector<int16_t> block(pass * 2);
    const size_t frames = stereo.size() / 2;
    for (size_t at = 0; at < frames; at += pass) {
        const size_t n = std::min(pass, frames - at);
        std::copy(stereo.begin() + static_cast<long>(at * 2), stereo.begin() + static_cast<long>((at + n) * 2), block.begin());
        std::fill(block.begin() + static_cast<long>(n * 2), block.end(), static_cast<int16_t>(0));
        int written = 0;
        const unsigned char* data = shine_encode_buffer_interleaved(encoder, block.data(), &written);
        if (data && written > 0) out.insert(out.end(), data, data + written);
    }
    int written = 0;
    const unsigned char* data = shine_flush(encoder, &written);
    if (data && written > 0) out.insert(out.end(), data, data + written);
    shine_close(encoder);
    return out;
}

}  // namespace plumnin
