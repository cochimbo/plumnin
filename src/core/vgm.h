#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plumnin {

class SynthMusic;
namespace gb {
class Apu;
}

// VGM is the usual file for game music as the sound chip played it: every register
// write with the waits between them, at 44100 ticks a second (vgmrips.net has the music
// of many Game Boy games). The instrument editor records a game to one, Sacar canciones
// writes each song of a game to one, and both play them back. For now, the Game Boy's
// (and the Color's) sound chip, VGM's "GB DMG".
class VgmWriter {
public:
    // Starts at `clock` (of a clock at `clockHz`), writing the chip's registers as they
    // are now (`regs` FF10-FF3F, wave RAM included) so the file sounds the same from its
    // first note.
    void begin(const uint8_t* regs, uint64_t clock, double clockHz);
    bool recording() const { return recording_; }
    // A write to the chip's register FF10 + `reg`.
    void gbWrite(int reg, uint8_t value, uint64_t clock);
    double seconds() const { return static_cast<double>(samples_) / 44100.0; }

    // A point of the recording: to end the file there, or to loop back to it.
    struct Mark {
        size_t bytes = 0;
        uint64_t samples = 0;
    };
    Mark mark(uint64_t clock);
    struct Tag {
        std::string title, game, notes;
    };
    // The file, gzip-compressed (.vgz), ending at `end` (at the last write or mark when
    // null) and looping back to `loop` (when not null).
    std::vector<uint8_t> finish(const Tag& tag, const Mark* end = nullptr, const Mark* loop = nullptr);
    // The commands so far, for comparing.
    const std::vector<uint8_t>& commands() const { return data_; }

private:
    void wait(uint64_t clock);

    bool recording_ = false;
    std::vector<uint8_t> data_;
    uint64_t start_ = 0;
    uint64_t samples_ = 0;
    double clockHz_ = 4194304.0;
};

// A VGM file read into memory: what is needed to play it.
struct VgmFile {
    std::vector<uint8_t> data;  // uncompressed
    size_t start = 0;           // the first command
    size_t loop = 0;            // where it loops back to, 0 for none
    uint32_t samples = 0, loopSamples = 0;
    uint32_t gbClock = 0;
    std::string title, game;
    // A .vgm or .vgz. False (and why, in Spanish) if it is not one this can play.
    bool parse(std::vector<uint8_t> bytes, std::string* error);
    double seconds() const { return samples / 44100.0; }
};

// Plays a VGM on a sound chip of its own, without the game: its sound, and through the
// synthesizer when given one, as the console would.
class VgmPlayer {
public:
    VgmPlayer();
    ~VgmPlayer();
    void load(VgmFile file);
    const VgmFile& file() const { return file_; }
    void restart();
    // The synthesizer (not owned; null for the chip's own sound) and how loud the chip
    // stays under it.
    void setSynth(SynthMusic* synth, int chipPercent);
    // `seconds` more of the music, at `rate`, added to `out` as interleaved stereo.
    void play(double seconds, double rate, std::vector<int16_t>& out);
    double position() const { return static_cast<double>(ticks_) / 44100.0; }
    bool finished() const { return finished_; }

private:
    void runTo(uint64_t tick);
    void runChip(uint64_t cycles);

    VgmFile file_;
    std::unique_ptr<gb::Apu> apu_;
    SynthMusic* synth_ = nullptr;
    int chipPercent_ = 0;
    size_t at_ = 0;
    uint64_t ticks_ = 0;    // 44100 Hz, played so far
    uint64_t waitUntil_ = 0;
    uint64_t cycles_ = 0;   // the chip's, at 4 MHz
    uint64_t sequencer_ = 0;  // cycles left to the next frame sequencer clock
    bool finished_ = false;
};

}  // namespace plumnin
