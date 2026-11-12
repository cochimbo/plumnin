#pragma once

#include <array>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "../core/pad.h"
#include "../core/system.h"
#include "../render/renderer.h"
#include "input.h"

namespace plumnin {

// Everything the menus change apart from the controls. Lives in plumnin.ini next to
// the [keyboard] / [gamepad] sections, under [interface], [video], [audio], [console],
// [speed], [states], [library], [network], [achievements], [screenscraper] and [recent].
// Ready-made widths for the widescreen: the columns on each side (at 256 pixels a line,
// wideColumnsFor) that make the picture that aspect, and its name.
struct Widescreen {
    int columns;
    const char* name;
};
constexpr Widescreen kWidescreens[] = {{0, "4:3"}, {26, "16:10"}, {43, "16:9"}};
constexpr int kWidescreenCount = 3;

struct Settings {
    static constexpr size_t kMaxRecent = 10;
    static constexpr int kMaxRewindSeconds = 60;

    std::string language;  // "es", "en", "fr" or "pt"; empty: the system's, decided at start
    // Console mode, for PCs played from the sofa with a pad (Steam Deck, Big Picture): full
    // screen, no menu bar, the library at start, bigger letters and the pad in every window.
    // 0 automatic (on when Steam's game mode or Big Picture started plumnin), 1 on, 2 off.
    int consoleMode = 0;
    // The size of the interface in percent (kMinUiScale-kMaxUiScale); 0 automatic: 100,
    // or after the screen's height in console mode.
    static constexpr int kMinUiScale = 75, kMaxUiScale = 250;
    int uiScale = 0;
    std::string view = "2D";  // Renderer::name() of the active view
    RenderOptions render;
    // Draw every sprite on a line, without the console's limit that makes them flicker.
    bool unlimitedSprites = false;
    // Widescreen: columns added on the left and on the right (at 256 pixels a line, up to
    // VideoFrame::kMaxWideSide), with the planes and sprites past the edges of the screen.
    int wideLeft = 0, wideRight = 0;
    int windowScale = 3;
    bool fullscreen = false;
    bool showMenu = true;
    int volume = 80;  // 0-100
    bool mute = false;
    Region region = Region::Auto;
    // What is in the second port (the first always has a pad).
    std::array<PadType, 2> padType{PadType::Pad, PadType::Pad};
    // The console's processor speed in percent (100 off, up to kMaxOverclock), for games
    // that slow down. In a netplay game the host's counts; hardcore mode keeps it at 100.
    static constexpr int kMaxOverclock = 400;  // Console::kMaxOverclock
    int overclock = 100;
    int stateSlot = 0;  // 0-9
    int rewindSeconds = 20;  // how far back rewinding reaches, 0 = off
    int fastForward = 4;     // fast-forward speed, times normal (2-10)
    int slowMotion = 50;     // slow-motion speed, percent of normal (10-90)
    // Run-ahead: frames of input lag taken away by showing the picture that many frames
    // ahead (0 off, up to kMaxRunAhead).
    static constexpr int kMaxRunAhead = 4;
    int runAhead = 0;
    // The frames-per-second counter over the game, with its graph, and the corner it is
    // in: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
    static constexpr int kFpsCorners = 4;
    bool fpsCounter = false;
    int fpsCorner = 3;
    std::vector<std::string> recent;  // most recent first
    std::string lastFolder;
    std::vector<std::string> libraryFolders;  // where the game library looks for games
    bool libraryAtStart = true;   // open the library when plumnin starts without a ROM
    int librarySystem = -1;       // the library's tab: -1 every console, else a ConsoleSystem
    bool libraryList = false;     // the library as a list (title, console, year...) instead of a grid of covers
    bool libraryDetails = true;   // the panel on the right with the chosen game's cover, data and description
    static constexpr int kMinCoverWidth = 90, kMaxCoverWidth = 300;
    int libraryCoverWidth = 150;  // the width of the grid's covers, in pixels: smaller ones fit more to a row
    static constexpr int kLibrarySorts = 5;
    int librarySort = 0;          // what the games are sorted by: 0 title, 1 console, 2 year, 3 genre, 4 developer
    // Netplay: the UDP port to host on (and to join when the address has none), the
    // last host joined and the input delay in frames.
    static constexpr int kDefaultNetPort = 7845;
    int netPort = kDefaultNetPort;
    std::string netAddress;
    int netInputDelay = 2;
    // RetroAchievements: on or off, the account (its token, never the password) and
    // hardcore mode.
    bool achievements = false;
    std::string raUser;
    std::string raToken;
    bool raHardcore = false;
    // ScreenScraper (screenscraper.fr), for the library's media: the user's account, a
    // developer account if plumnin was built without one, and the kinds of media to
    // download (SsKind ids with commas; empty: the usual ones).
    std::string ssUser, ssPassword;
    std::string ssDevId, ssDevPassword;
    std::string ssMedia;
    // Downloading a game again overwrites what it has (else only what is missing is added).
    bool ssOverwrite = false;

    void addRecent(const std::string& path);
    void removeRecent(const std::string& path);

    // Reads the sections it knows from `path`, ignoring the rest.
    static Settings load(const std::string& path);
    // Applies the sections it knows from `in` onto `s`, leaving the rest of `s` as it is.
    static void parse(std::istream& in, Settings& s);
    // Rewrites the whole file: the controls from `input` plus these settings.
    bool save(const std::string& path, const InputConfig& input) const;
    // Writes every section of these settings (not the controls), with comments.
    void write(std::ostream& out) const;
};

}  // namespace plumnin
