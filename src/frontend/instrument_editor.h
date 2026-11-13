#pragma once

#include <cstdint>
#include <string>

#include "../core/synth_music.h"

namespace plumnin {

// The instrument editor (Sonido > Editor de instrumentos): while the game plays through
// the synthesizer, it lists the instruments heard so far, what each sounds like on the
// chip and what the rules give it, and lets each one be given another General MIDI
// instrument (with its octave and volume), a piece of the drum kit, or its own sound
// from the chip. "Oír solo este" silences the rest. What is chosen is heard at once and
// kept per game in sound/<game>.ini, in plumsg's format.
class InstrumentEditor {
public:
    // The sound folder, ending in a separator.
    void setFolder(std::string folder) { folder_ = std::move(folder); }
    // What a game (its path) has chosen, from its file; none if it has none.
    static SynthMusic::Assignments load(const std::string& folder, const std::string& game);
    static std::string fileFor(const std::string& folder, const std::string& game);

    bool isOpen() const { return open_; }
    void open() { open_ = true; }
    // Draws the window, between ImGui::NewFrame and ImGui::Render. `game` is the path
    // of the game running (empty for none), `on` whether the synthesizer is playing it.
    void draw(SynthMusic& synth, const std::string& game, bool on);

private:
    void assign(SynthMusic& synth, uint32_t fingerprint, const SynthMusic::Assignment* a);  // null: the rules'
    void save(const SynthMusic& synth);
    void solo(SynthMusic& synth, uint32_t fingerprint);  // 0: everything heard again

    std::string folder_;
    std::string game_;
    std::string error_;
    bool open_ = false;
    uint32_t solo_ = 0;
};

}  // namespace plumnin
