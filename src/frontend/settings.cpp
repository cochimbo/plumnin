#include "settings.h"

#include "../core/ini_names.h"
#include "../core/system.h"
#include "../core/video_frame.h"
#include "../render/profile3d.h"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace plumnin {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// A port's type as plumnin.ini writes it.
const char* padIni(PadType t) { return t == PadType::None ? "none" : "pad"; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool yes(const std::string& v) {
    const std::string l = lower(v);
    return l == "1" || iniName(l) == "yes" || l == "true";
}

constexpr const char* kScaleNames[] = {"integer", "fit", "stretch"};
constexpr const char* kFilterNames[] = {"none", "scanlines", "crt"};
constexpr const char* kUpscaleNames[] = {"none", "xbr", "anime4k"};
constexpr const char* kRegionNames[] = {"auto", "japan", "usa", "europe"};
constexpr const char* kSystemKeys[] = {"gameboy", "gameboycolor", "nes", "snes"};
constexpr const char* kLibrarySortKeys[] = {"title", "console", "year", "genre", "developer"};
static_assert(std::size(kLibrarySortKeys) == Settings::kLibrarySorts, "one key per order");
static_assert(std::size(kSystemKeys) == kSystemCount, "one key per console");

}  // namespace

void Settings::addRecent(const std::string& path) {
    removeRecent(path);
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) lastFolder = path.substr(0, slash);
}

void Settings::removeRecent(const std::string& path) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
}

Settings Settings::load(const std::string& path) {
    Settings s;
    std::ifstream in(path);
    parse(in, s);
    return s;
}

void Settings::parse(std::istream& in, Settings& s) {
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = iniName(lower(trim(line.substr(1, line.size() - 2))));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = iniName(lower(trim(line.substr(0, eq))));
        const std::string value = trim(line.substr(eq + 1));

        if (section == "interface") {
            if (key == "language") s.language = lower(value);
            else if (key == "console_mode") {
                const std::string v = iniName(lower(value));
                s.consoleMode = v == "yes" ? 1 : v == "no" ? 2 : 0;
            } else if (key == "size") {
                const int v = std::atoi(value.c_str());
                s.uiScale = v <= 0 ? 0 : std::clamp(v, kMinUiScale, kMaxUiScale);
            }
        } else if (section == "video") {
            if (key == "view") s.view = value;
            else if (key == "scale") {
                for (int i = 0; i < 3; ++i)
                    if (iniName(lower(value)) == kScaleNames[i]) s.render.scale = static_cast<ScaleMode>(i);
            } else if (key == "smooth") s.render.smooth = yes(value);
            else if (key == "filter") {
                for (int i = 0; i < 3; ++i)
                    if (iniName(lower(value)) == kFilterNames[i]) s.render.filter = static_cast<Filter2D>(i);
            } else if (key == "filter_strength") s.render.filterStrength = std::clamp(std::atoi(value.c_str()), 0, 100) / 100.0f;
            else if (key == "upscaling") {
                for (int i = 0; i < 3; ++i)
                    if (iniName(lower(value)) == kUpscaleNames[i]) s.render.upscale = static_cast<Upscale2D>(i);
            } else if (read3DKey(key, value, s.render)) continue;
            else if (key == "square_pixels") s.render.squarePixels = yes(value);
            else if (key == "no_sprite_limit") s.unlimitedSprites = yes(value);
            else if (key == "widescreen") {  // as the first version wrote it: no, 16:10 or 16:9
                s.wideLeft = s.wideRight = 0;
                for (const Widescreen& w : kWidescreens)
                    if (iniName(lower(value)) == w.name) s.wideLeft = s.wideRight = w.columns;
            } else if (key == "widescreen_left") s.wideLeft = std::clamp(std::atoi(value.c_str()), 0, VideoFrame::kMaxWideSide);
            else if (key == "widescreen_right") s.wideRight = std::clamp(std::atoi(value.c_str()), 0, VideoFrame::kMaxWideSide);
            else if (key == "window_scale") s.windowScale = std::clamp(std::atoi(value.c_str()), 1, 8);
            else if (key == "fullscreen") s.fullscreen = yes(value);
            else if (key == "menu_bar") s.showMenu = yes(value);
        } else if (section == "audio") {
            if (key == "volume") s.volume = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "mute") s.mute = yes(value);
            else if (key == "synth") s.synth = yes(value);
            else if (key == "soundfont") s.soundfont = value;
            else if (key == "synth_chip") s.chipMix = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "reverb") s.reverb = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "chorus") s.chorus = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (section == "console") {
            if (key == "region") {
                for (int i = 0; i < 4; ++i)
                    if (iniName(lower(value)) == kRegionNames[i]) s.region = static_cast<Region>(i);
            } else if (key == "pad2") {  // pad 1 is always there
                s.padType[1] = lower(value) == "none" ? PadType::None : PadType::Pad;
            } else if (key == "overclock") {
                s.overclock = std::clamp(std::atoi(value.c_str()), 100, kMaxOverclock);
            }
        } else if (section == "speed") {
            if (key == "rewind_seconds") s.rewindSeconds = std::clamp(std::atoi(value.c_str()), 0, kMaxRewindSeconds);
            else if (key == "fast_forward") s.fastForward = std::clamp(std::atoi(value.c_str()), 2, 10);
            else if (key == "slow_motion") s.slowMotion = std::clamp(std::atoi(value.c_str()), 10, 90);
            else if (key == "run_ahead") s.runAhead = std::clamp(std::atoi(value.c_str()), 0, kMaxRunAhead);
            else if (key == "fps_counter") s.fpsCounter = yes(value);
            else if (key == "fps_counter_corner") s.fpsCorner = std::clamp(std::atoi(value.c_str()), 0, kFpsCorners - 1);
        } else if (section == "states") {
            if (key == "slot") s.stateSlot = std::clamp(std::atoi(value.c_str()), 0, 9);
        } else if (section == "library") {
            if (key == "folder") {
                if (!value.empty() && std::find(s.libraryFolders.begin(), s.libraryFolders.end(), value) == s.libraryFolders.end())
                    s.libraryFolders.push_back(value);
            } else if (key == "open_at_start") s.libraryAtStart = yes(value);
            else if (key == "view") {
                const std::string view = iniName(lower(value));
                s.libraryList = view == "list";
            }
            else if (key == "details") s.libraryDetails = yes(value);
            else if (key == "cover_width")
                s.libraryCoverWidth = std::clamp(std::atoi(value.c_str()), Settings::kMinCoverWidth, Settings::kMaxCoverWidth);
            else if (key == "sort") {
                s.librarySort = 0;
                for (int i = 0; i < Settings::kLibrarySorts; ++i)
                    if (iniName(lower(value)) == kLibrarySortKeys[i]) s.librarySort = i;
            } else if (key == "system") {
                s.librarySystem = -1;
                for (int i = 0; i < kSystemCount; ++i)
                    if (iniName(lower(value)) == kSystemKeys[i]) s.librarySystem = i;
            }
        } else if (section == "network") {
            if (key == "port") s.netPort = std::clamp(std::atoi(value.c_str()), 1, 65535);
            else if (key == "address") s.netAddress = value;
            else if (key == "input_delay") s.netInputDelay = std::clamp(std::atoi(value.c_str()), 0, 6);
        } else if (section == "achievements") {
            if (key == "enabled") s.achievements = yes(value);
            else if (key == "user") s.raUser = value;
            else if (key == "token") s.raToken = value;
            else if (key == "hardcore") s.raHardcore = yes(value);
        } else if (section == "screenscraper") {
            if (key == "user") s.ssUser = value;
            else if (key == "password") s.ssPassword = value;
            else if (key == "dev_id") s.ssDevId = value;
            else if (key == "dev_password") s.ssDevPassword = value;
            else if (key == "media") s.ssMedia = value;
            else if (key == "overwrite") s.ssOverwrite = yes(value);
        } else if (section == "recent") {
            if (key == "rom" && !value.empty() && s.recent.size() < kMaxRecent) s.recent.push_back(value);
            else if (key == "last_folder") s.lastFolder = value;
        }
    }
}

bool Settings::save(const std::string& path, const InputConfig& input) const {
    std::ofstream out(path);
    if (!out) return false;
    input.write(out);
    write(out);
    return static_cast<bool>(out);
}

void Settings::write(std::ostream& out) const {
    auto flag = [](bool b) { return b ? "yes" : "no"; };
    out << "\n[interface]\n"
        << "# language: es (español), en (English), fr (français) or pt (português); empty: the system's.\n"
        << "language = " << language << "\n"
        << "# console_mode: auto (on in Steam's game mode or Big Picture), yes or no. Full screen,\n"
        << "# no menu bar, the library at start, bigger letters and the pad in every window.\n"
        << "# size: the size of the interface in percent (75 to 250); auto: after the screen in console mode.\n"
        << "console_mode = " << (consoleMode == 1 ? "yes" : consoleMode == 2 ? "no" : "auto") << "\n"
        << "size = ";
    if (uiScale > 0) out << uiScale;
    else out << "auto";
    out << "\n"
        << "\n[video]\n"
        << "# view: 2D or a debug view. scale: integer, fit or stretch.\n"
        << "view = " << view << "\n"
        << "scale = " << kScaleNames[static_cast<int>(render.scale)] << "\n"
        << "smooth = " << flag(render.smooth) << "\n"
        << "# filter for the 2D view: none, scanlines or crt. filter_strength: 0 to 100.\n"
        << "filter = " << kFilterNames[static_cast<int>(render.filter)] << "\n"
        << "filter_strength = " << static_cast<int>(render.filterStrength * 100.0f + 0.5f) << "\n"
        << "# upscaling (experimental) of the 2D view: none, xbr or anime4k. It replaces the filter.\n"
        << "upscaling = " << kUpscaleNames[static_cast<int>(render.upscale)] << "\n";
    write3D(out, render);
    out << "# square_pixels: do not stretch the picture to 4:3 like a TV.\n"
        << "square_pixels = " << flag(render.squarePixels) << "\n"
        << "# no_sprite_limit: draw every sprite on a line, without the console's limit that makes\n"
        << "# them flicker or vanish. The game does not notice.\n"
        << "no_sprite_limit = " << flag(unlimitedSprites) << "\n"
        << "# widescreen_left/right: extra columns on each side (0 to 56; 43 and 43 is 16:9).\n"
        << "# They show what lies beyond the edges; in some games you see things you were not meant to.\n"
        << "widescreen_left = " << wideLeft << "\n"
        << "widescreen_right = " << wideRight << "\n"
        << "window_scale = " << windowScale << "\n"
        << "fullscreen = " << flag(fullscreen) << "\n"
        << "menu_bar = " << flag(showMenu) << "\n"
        << "\n[audio]\n"
        << "# volume: 0 to 100.\n"
        << "volume = " << volume << "\n"
        << "mute = " << flag(mute) << "\n"
        << "# synth: the music as General MIDI instruments on a SoundFont (soundfont, a .sf2; empty for\n"
        << "# the included GeneralUser GS). synth_chip: how loud the chip's own music stays under it,\n"
        << "# reverb and chorus: 0 to 100.\n"
        << "synth = " << flag(synth) << "\n"
        << "soundfont = " << soundfont << "\n"
        << "synth_chip = " << chipMix << "\n"
        << "reverb = " << reverb << "\n"
        << "chorus = " << chorus << "\n"
        << "\n[console]\n"
        << "# region: auto (the cartridge's), japan, usa or europe. pad2: pad, or none for nothing plugged in.\n"
        << "region = " << kRegionNames[static_cast<int>(region)] << "\n"
        << "pad2 = " << padIni(padType[1]) << "\n"
        << "# overclock: the console's processor speed in percent, from 100 (the console's) to 400.\n"
        << "# It removes slowdown in some games; a few may run too fast or fail.\n"
        << "overclock = " << overclock << "\n"
        << "\n[speed]\n"
        << "# rewind_seconds: how many seconds can be rewound (0 turns it off, up to 60).\n"
        << "# fast_forward: times faster, 2 to 10. slow_motion: percent of the speed, 10 to 90.\n"
        << "# run_ahead: frames of input lag removed, 0 (off) to 4.\n"
        << "rewind_seconds = " << rewindSeconds << "\n"
        << "fast_forward = " << fastForward << "\n"
        << "slow_motion = " << slowMotion << "\n"
        << "run_ahead = " << runAhead << "\n"
        << "# fps_counter: the frames per second counter with its graph (F key).\n"
        << "# fps_counter_corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right.\n"
        << "fps_counter = " << flag(fpsCounter) << "\n"
        << "fps_counter_corner = " << fpsCorner << "\n"
        << "\n[states]\n"
        << "# slot: the active state slot, 0 to 9.\n"
        << "slot = " << stateSlot << "\n"
        << "\n[library]\n"
        << "# folder: where your games are (one line per folder). open_at_start: show the library when\n"
        << "# plumnin opens without a game. system: the chosen tab (all, gameboy, gameboycolor, nes or snes).\n"
        << "# view: grid or list. details: the right-hand panel with the chosen game's data.\n"
        << "# sort: title, console, year, genre or developer. cover_width: the grid's covers, in pixels.\n";
    for (const auto& f : libraryFolders) out << "folder = " << f << "\n";
    out << "open_at_start = " << flag(libraryAtStart) << "\n"
        << "system = " << (librarySystem >= 0 && librarySystem < kSystemCount ? kSystemKeys[librarySystem] : "all") << "\n"
        << "view = " << (libraryList ? "list" : "grid") << "\n"
        << "details = " << flag(libraryDetails) << "\n"
        << "cover_width = " << libraryCoverWidth << "\n"
        << "sort = " << kLibrarySortKeys[std::clamp(librarySort, 0, kLibrarySorts - 1)] << "\n"
        << "\n[network]\n"
        << "# port: the UDP port for hosting games. address: the last host you joined.\n"
        << "# input_delay: frames between pressing a button and it taking effect in netplay (0 to 6).\n"
        << "port = " << netPort << "\n"
        << "address = " << netAddress << "\n"
        << "input_delay = " << netInputDelay << "\n"
        << "\n[achievements]\n"
        << "# RetroAchievements. enabled: yes or no. user and token: the account (the token stands in for\n"
        << "# the password, which is not saved). hardcore: no rewind, slow motion, cheats or loading states.\n"
        << "enabled = " << flag(achievements) << "\n"
        << "user = " << raUser << "\n"
        << "token = " << raToken << "\n"
        << "hardcore = " << flag(raHardcore) << "\n"
        << "\n[screenscraper]\n"
        << "# The screenscraper.fr account for the library's media (ScreenScraper asks for the password\n"
        << "# with every request, so it is kept here). dev_id and dev_password: a developer account, only\n"
        << "# if this build of plumnin has none of its own. media: the kinds to download, with commas.\n"
        << "# overwrite: downloading again replaces every file (no: only what is missing is added).\n"
        << "user = " << ssUser << "\n"
        << "password = " << ssPassword << "\n"
        << "dev_id = " << ssDevId << "\n"
        << "dev_password = " << ssDevPassword << "\n"
        << "media = " << ssMedia << "\n"
        << "overwrite = " << flag(ssOverwrite) << "\n"
        << "\n[recent]\n";
    if (!lastFolder.empty()) out << "last_folder = " << lastFolder << "\n";
    for (const auto& r : recent) out << "rom = " << r << "\n";
}

}  // namespace plumnin
