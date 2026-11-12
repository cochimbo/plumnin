#pragma once

#include <set>
#include <string>
#include <vector>

#include "input.h"
#include "settings.h"

namespace plumnin {

// Options a game can keep a value of its own for, which takes the place of the general
// one while that game is loaded. Each game's own values live in a small .ini of that game
// in the profiles folder: the 3D options in [profile_3d] (the 3D profiles of before) and
// the rest in the same sections and with the same keys as in plumnin.ini.
struct GameOption {
    const char* key;    // the key in the .ini; "profile_3d" stands for every 3D option, "pads" for both pads
    const char* label;  // for the Opciones de este juego window
    const char* group;  // Vídeo, 3D, Sonido, Consola, Velocidad
    void (*copy)(const Settings& from, Settings& to);
};
const std::vector<GameOption>& gameOptions();
const GameOption* findGameOption(const std::string& key);

// The key in GameOverrides::own for a game's own console buttons (keyboard and pads),
// kept in its file under [teclado], [teclado2] and [mando] as in plumnin.ini.
constexpr const char* kGameControls = "controls";

struct GameOverrides {
    std::set<std::string> own;  // keys of gameOptions() this game has its own value for, and kGameControls
    Settings values;            // those values; the rest are the general ones
    InputConfig controls;       // its console buttons when it has its own (hotkeys stay general)

    bool has(const std::string& key) const { return own.count(key) != 0; }
    // Copies the game's own values onto `to`.
    void applyTo(Settings& to) const;
    // Sets `to` back to the general values (from `general`) wherever this game had its own.
    void restore(const Settings& general, Settings& to) const;
};

// Reads a game's file: `values` start as `general` (and `controls` as `generalInput`)
// and take the file's keys on top.
GameOverrides parseGameOverrides(const std::string& text, const Settings& general, const InputConfig& generalInput);
// The file for `o`, or an empty string when the game has nothing of its own.
std::string formatGameOverrides(const GameOverrides& o, const std::string& game);

}  // namespace plumnin
