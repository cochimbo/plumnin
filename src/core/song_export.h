#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace plumnin {

struct VgmFile;
class SynthMusic;

// Exportar a MP3: a song as the instrument editor plays it (the synthesizer with what was
// chosen for it, and the chip under it), to a file to keep or share.
struct SongRender {
    int rate = 44100;
    int loops = 2;            // times round its loop, after its start
    double fadeSeconds = 8;   // then it fades out (a song that ends just ends)
    double maxSeconds = 600;  // the longest it can be, fade included
    // An SPC plays on for ever: as long as its tag says, or this.
    double spcSeconds = 150;
};

// How long the export of `file` lasts, its fade included.
double renderedSeconds(const VgmFile& file, const SongRender& how);
// Plays `file` through `synth` (null: the chip's own sound) with the chip at
// `chipPercent` under it, into `out` (interleaved stereo at how.rate). `progress`, if
// given, goes from 0 to 1; `cancel` stops it (false is returned).
bool renderSong(const VgmFile& file, SynthMusic* synth, int chipPercent, const SongRender& how, std::vector<int16_t>* out,
                std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);

struct Mp3Tag {
    std::string title, album, artist;
    int track = 0;  // 0 for none
};
// Interleaved stereo at `rate` (44100, 48000 or 32000) to an MP3 at `kbps`, with an
// ID3 tag. Empty if the rate or the bit rate is not one MP3 has.
std::vector<uint8_t> encodeMp3(const std::vector<int16_t>& stereo, int rate, int kbps, const Mp3Tag& tag);

}  // namespace plumnin
