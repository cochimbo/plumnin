#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/songs.h"
#include "../core/synth_music.h"
#include "../core/vgm.h"

namespace plumnin {

class Console;

// The instrument editor (Sonido > Editor de instrumentos): while the game plays through
// the synthesizer, it lists the instruments heard so far, what each sounds like on the
// chip and what the rules give it, and lets each one be given another General MIDI
// instrument (with its octave and volume), a piece of the drum kit, or its own sound
// from the chip. "Oír solo este" silences the rest. What is chosen is heard at once and
// kept per game in sound/<game>.ini, in plumsg's format.
//
// Its other tab, Canciones, has the game's songs as VGM files in sound/tracks/<game>/,
// to hear them without playing (the game waits meanwhile), through the synthesizer
// when it is on, so that its instruments can be changed while they play. They come
// from recording the game as it plays, or, on the Game Boy, from Sacar canciones,
// which finds every song in the ROM by itself (SongRipper).
class InstrumentEditor {
public:
    InstrumentEditor() = default;
    ~InstrumentEditor();
    InstrumentEditor(const InstrumentEditor&) = delete;
    InstrumentEditor& operator=(const InstrumentEditor&) = delete;

    // The sound folder, ending in a separator.
    void setFolder(std::string folder) { folder_ = std::move(folder); }
    // What a game (its path) has chosen, from its file; none if it has none.
    static SynthMusic::Assignments load(const std::string& folder, const std::string& game);
    static std::string fileFor(const std::string& folder, const std::string& game);

    bool isOpen() const { return open_; }
    void open() { open_ = true; }
    // Draws the window, between ImGui::NewFrame and ImGui::Render. `game` is the path
    // of the game running (empty for none) and `console` its console, `on` whether the
    // synthesizer is playing it, and `chipPercent` how loud the chip stays under it.
    void draw(SynthMusic& synth, const std::string& game, bool on, Console* console, int chipPercent);

    // A song is playing: the game waits, and its sound is play()'s.
    bool playing() const { return playing_; }
    void play(double seconds, double rate, std::vector<int16_t>& out);
    // The folder of a game's songs.
    static std::string tracksFor(const std::string& folder, const std::string& game);

private:
    void drawInstruments(SynthMusic& synth, bool on);
    void drawSongs(SynthMusic& synth, bool on, Console* console, int chipPercent);
    void scanSongs();
    void startSong(size_t index, SynthMusic& synth, bool on, int chipPercent);
    void stopSong();
    void stopRecording(Console* console, bool keep);
    void finishRip();

    void assign(SynthMusic& synth, uint32_t fingerprint, const SynthMusic::Assignment* a);  // null: the rules'
    void save(const SynthMusic& synth);
    void solo(SynthMusic& synth, uint32_t fingerprint);  // 0: everything heard again

    std::string folder_;
    std::string game_;
    std::string error_;
    bool open_ = false;
    uint32_t solo_ = 0;

    struct Track {
        std::string name, path;
        double seconds = 0, loopSeconds = 0;
    };
    std::vector<Track> songs_;
    bool scanned_ = false;
    VgmPlayer player_;
    bool playing_ = false;
    size_t current_ = 0;
    SynthMusic* playSynth_ = nullptr;
    // Recording the game's music.
    VgmWriter writer_;
    Console* recording_ = nullptr;
    // Sacar canciones, on a thread of its own.
    std::thread ripThread_;
    std::unique_ptr<SongRipper::Progress> ripProgress_;
    std::atomic<bool> ripDone_{false};
    bool ripOk_ = false;
    std::vector<SongRipper::Song> ripSongs_;
    std::string ripError_, ripGame_, message_;
};

}  // namespace plumnin
