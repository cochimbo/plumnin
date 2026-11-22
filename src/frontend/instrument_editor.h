#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/songs.h"
#include "../core/synth_music.h"
#include "../core/system.h"
#include "../core/vgm.h"

namespace plumnin {

class Console;

// What the editor needs to know about the emulator each frame.
struct EditorView {
    std::string game;            // the game it is for (its path), empty for none
    Console* console = nullptr;  // that game's console, when it is the one running
    SynthMusic* gameSynth = nullptr;  // the console's synthesizer: its SoundFont, and what it hears
    bool synthOn = false;        // the game's music goes through it
    int chipPercent = 0, reverb = 40, chorus = 20;
    bool effectsOriginal = true;  // the game's sound effects keep the chip's sound
    std::function<void()> drawSoundfont;  // the SoundFont drop-down, next to Sintetizador
};

// What the editor asks of the emulator.
struct EditorRequests {
    bool chooseTrack = false;  // pick a .vgm to open (then openTrack())
    std::string load;          // play this game (to record it)
};

// The instrument editor (Sonido > Editor de instrumentos, or a game's menu in the
// library), as plumsg's: plays a song of the game (taken out of the ROM with Sacar
// canciones, recorded while playing, or opened from a .vgm), lists the instruments in it
// and its channels, and lets each instrument be given a General MIDI one, a piece of the
// drum kit or its own sound, comparing with the original as it plays. With no song open
// and the game running through the synthesizer, the instruments heard so far.
//
// What is chosen is kept for the song alone (sound/tracks/<game>/<song>.ini), or for the
// whole game in sound/<game>.ini, in plumsg's format, and heard at once in the game: a
// song's choices while the game plays that song, told by its instruments. Its songs are in sound/tracks/<game>/; the instruments of its sound
// effects (Sacar canciones finds them), in sound/<game>.effects.
class InstrumentEditor {
public:
    InstrumentEditor();
    ~InstrumentEditor();
    InstrumentEditor(const InstrumentEditor&) = delete;
    InstrumentEditor& operator=(const InstrumentEditor&) = delete;

    // The sound folder, ending in a separator.
    void setFolder(std::string folder) {
        folder_ = std::move(folder);
        ++musicVersion_;
    }
    // What a game (its path) has chosen, from its file; none if it has none.
    static SynthMusic::Assignments load(const std::string& folder, const std::string& game);
    static std::string fileFor(const std::string& folder, const std::string& game);
    // A game's songs: what was chosen for each one alone, and its instruments (for telling
    // which one the game plays). In sound/tracks/<game>/<song>.ini, next to each song.
    static std::vector<SynthMusic::Song> songsFor(const std::string& folder, const std::string& game);
    // A game's sound effects, as Sacar canciones found them (none if it was not run).
    static std::set<uint32_t> effectsFor(const std::string& folder, const std::string& game);

    // The games with songs in their track folder (sound/tracks/<game>/), by file name
    // without extension, and how many: the library marks them with a note.
    const std::map<std::string, int>& gamesWithMusic();

    bool isOpen() const { return open_; }
    void open() { open_ = true; }
    // Draws the window, between ImGui::NewFrame and ImGui::Render.
    void draw(const EditorView& view, EditorRequests& out);

    // A song is playing: the game waits, and its sound is play()'s.
    bool playing() const { return open_ && playing_; }
    void play(double seconds, double rate, std::vector<int16_t>& out);
    // Opens a .vgm, .vgz or .spc, or a .zip of them (a vgmrips pack). A file from elsewhere is
    // copied into the game's track folder, so it is there next time.
    bool openTrack(const std::string& path);
    // The folder of a game's songs.
    static std::string tracksFor(const std::string& folder, const std::string& game);

private:
    struct Instrument {
        uint32_t fingerprint = 0;
        SynthMusic::Seen seen;
        bool heard = false;  // in this song (else only in the game's file)
        std::string label;   // "Pulso A", "Ruido B"...
    };
    struct Track {
        std::string path;
        std::string member;  // in a .zip; empty for a file
        std::string title;
        bool operator==(const Track& o) const { return path == o.path && member == o.member; }
    };

    void setGame(const EditorView& view);
    void listTracks();
    void addTracks(const std::string& path);
    bool loadTrack(const Track& track);
    void stop();
    static std::string songFile(const Track& track);  // what was chosen for it alone
    // A song's file: its choices, and the instruments in it.
    static void writeSong(const std::string& path, const SynthMusic::Assignments& assigned,
                          const std::set<uint32_t>& instruments);
    bool songOpen() const { return !live_ && player_.loaded() && !track_.path.empty(); }
    bool editingSong() const { return songOpen() && forSong_; }  // changes go to the song alone
    SynthMusic::Assignments chosen() const;  // the game's choices, and over them the song's
    SynthMusic::Assignments& target() { return editingSong() ? songOwn_ : own_; }
    std::vector<Instrument> instruments(const SynthMusic& synth) const;
    SynthMusic& listened(const EditorView& view);  // the synthesizer the lists are of
    void applyListening(const EditorView& view);
    void assign(const EditorView& view, uint32_t fingerprint, const SynthMusic::Assignment* a);
    void resetAll(const EditorView& view);
    void save(const EditorView& view);
    void drawTrack(const EditorView& view, EditorRequests& out);
    void drawInstruments(const EditorView& view, const std::vector<Instrument>& list);
    void drawChannels(const EditorView& view, const std::vector<Instrument>& list);
    void drawAssignment(const EditorView& view, const std::vector<Instrument>& list);
    void startRip();
    void drawRip();
    void finishRip();
    void stopRecording(Console* console, bool keep);
    void saveSnapshot(Console* console);  // the SNES's music now, as an .spc
    static int songsIn(const std::string& folder);  // .vgm, .vgz and .zip
    ConsoleSystem trackSystem() const;  // whose channels the lists show

    std::string folder_;
    bool open_ = false;
    std::string game_, gameName_;
    ConsoleSystem system_ = ConsoleSystem::GameBoy;
    bool live_ = false;  // listing what the game plays, not a song

    SynthMusic synth_;  // the editor's own, for the songs
    VgmPlayer player_;
    Track track_;
    std::vector<Track> tracks_;
    std::map<uint32_t, SynthMusic::Seen> trackSeen_;
    bool playing_ = false;
    bool synthOutput_ = true;
    bool loop_ = true;
    float seekTo_ = -1;
    int chipPercent_ = 0;
    SynthMusic::Assignments own_;      // for the whole game
    SynthMusic::Assignments songOwn_;  // for the song open alone
    bool forSong_ = true;              // what is changed goes to the song open, not the game
    uint32_t selected_ = 0;
    uint32_t mutedVoices_ = 0, soloVoices_ = 0;
    uint32_t soloInstrument_ = 0;
    std::set<uint32_t> silencedInstruments_;
    bool pendingSlide_ = false;
    std::string message_;
    int effectsVersion_ = 0, effectsSeen_ = 0;  // the .effects file was written again

    int musicVersion_ = 1, musicSeen_ = 0;  // songs changed: the library reads them again
    std::map<std::string, int> withMusic_;

    // Recording the game's music.
    VgmWriter writer_;
    Console* recording_ = nullptr;
    // Sacar canciones, on a thread of its own.
    std::thread ripThread_;
    std::unique_ptr<SongRipper::Progress> ripProgress_;
    std::atomic<bool> ripDone_{false};
    bool ripOk_ = false;
    std::vector<SongRipper::Song> ripSongs_;
    std::vector<std::set<uint32_t>> ripInstruments_;  // each song's
    std::set<uint32_t> ripEffects_;
    std::string ripError_, ripGame_;
};

}  // namespace plumnin
