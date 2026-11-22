#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cpu_tracer.h"
#include "system.h"

namespace plumnin {

// Sacar canciones: every song of a Game Boy (or Color) or NES game, each to a VGM with
// its loop, from the ROM alone (and of a Super Nintendo one, each to an SPC: see
// songs_snes.cpp). Games have no list of their songs, but every one has a
// sound driver: a routine called a few dozen times a second that writes the chip, and a
// way for the game to ask it for a song. So this runs the game for a while watching the
// processor, finds the driver and how the game asks (a routine it calls with the song's
// number, or a byte of memory it writes for the driver to read), then asks it for every
// number in turn with only the driver running, keeps what sounds like music on two or
// more channels, and records each one until the driver comes back to where it was.
class SongRipper {
public:
    struct Song {
        int number = 0;  // what the game asked for to get it
        std::vector<uint8_t> vgz;  // or, with `spc`, an SPC (the Super Nintendo's)
        bool spc = false;
        double seconds = 0, loopSeconds = 0;  // loopSeconds 0: it ends
        // How the game asks for it, with `number` (SongWatcher's format: "rutina 03:4A2B A",
        // "byte C0F1"); empty if it is not asked for so (the Super Nintendo's).
        std::string request;
    };
    // For a thread of its own: how far it is, read from another, and a way to stop it.
    struct Progress {
        std::atomic<int> step{0}, steps{1};
        std::atomic<int> found{0};
        std::atomic<bool> cancel{false};
    };
    struct Options {
        double traceSeconds = 25;     // how long the game runs while it is watched
        double probeSeconds = 3;      // how long each number is listened to
        double maxSeconds = 300;      // the longest a song is recorded, without a loop
    };

    static bool supported(ConsoleSystem system);
    // Runs it all: from a few seconds to a minute. False, with the reason in Spanish, if no song was
    // found. `report` says what it found and how, for the curious, and `effects` the
    // instruments (SynthMusic fingerprints) of the game's sound effects: those heard in
    // the short sounds it skipped and in none of its songs.
    static bool run(const std::vector<uint8_t>& rom, const Options& options, Progress* progress,
                    std::vector<Song>* songs, std::string* error, std::string* report = nullptr,
                    std::set<uint32_t>* effects = nullptr);
};

class Console;
class GameBoy;
class Nes;
class SynthMusic;

// Watches a Game Boy (or Color) or NES game as it is played for the requests for its songs
// that Sacar canciones found (SongRipper::Song::request and number): the routine called
// with a song's number, or the byte of memory written with it. The song the game asks for
// is told to the synthesizer, for what was chosen for that song alone.
class SongWatcher : public CpuTracer {
public:
    struct Request {
        std::string way;  // as SongRipper::Song::request
        int number = 0;
    };
    // False if the console is not one it can watch, or none of the requests makes sense.
    bool attach(Console* console, SynthMusic* synth, const std::vector<Request>& requests);
    void detach();
    // Forgets the console without touching it: it is gone already.
    void release() { console_ = nullptr; gb_ = nullptr; nes_ = nullptr; detach(); }
    bool attached() const { return console_ != nullptr; }

    void instruction(uint16_t, uint16_t) override {}
    void call(uint16_t to, uint16_t sp, bool interrupt) override;
    void access(uint16_t address, uint8_t value, bool write) override;

private:
    struct Way {
        bool routine = false;
        uint32_t id = 0;  // the routine (bank and address) or the byte's index in RAM
        int reg = 0;      // which register has the number: 0-2 (A, C, E or A, X, Y)
        std::set<int> numbers;
        std::string text;
    };
    Console* console_ = nullptr;
    GameBoy* gb_ = nullptr;  // the console, as one or the other
    Nes* nes_ = nullptr;
    SynthMusic* synth_ = nullptr;
    std::vector<Way> ways_;
    bool anyRoutine_ = false;
    std::set<int> bytes_;  // the RAM indices watched
};

}  // namespace plumnin
