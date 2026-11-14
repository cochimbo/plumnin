#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "system.h"

namespace plumnin {

// Sacar canciones: every song of a Game Boy (or Color) or NES game, each to a VGM with
// its loop, from the ROM alone. Games have no list of their songs, but every one has a
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
        std::vector<uint8_t> vgz;
        double seconds = 0, loopSeconds = 0;  // loopSeconds 0: it ends
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
    // found. `report` says what it found and how, for the curious.
    static bool run(const std::vector<uint8_t>& rom, const Options& options, Progress* progress,
                    std::vector<Song>* songs, std::string* error, std::string* report = nullptr);
};

}  // namespace plumnin
