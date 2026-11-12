#include "game_options.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "../core/i18n.h"
#include "../core/ini_names.h"
#include "../render/profile3d.h"

namespace plumnin {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The option a key of the file belongs to: both pads are one option, and so are both
// sides of the widescreen.
// The option a key of the file belongs to (keys are English, or Spanish from before 0.0.32).
std::string optionFor(const std::string& name) {
    const std::string& key = iniName(name);
    if (key == "pad1" || key == "pad2") return "pads";
    if (key == "widescreen_left" || key == "widescreen_right") return "widescreen";
    return key;
}

}  // namespace

const std::vector<GameOption>& gameOptions() {
    static const std::vector<GameOption> options = {
        {"view", N_("Vista"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.view = f.view; }},
        {"scale", N_("Escalado"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.scale = f.render.scale; }},
        {"smooth", N_("Suavizado"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.smooth = f.render.smooth; }},
        {"filter", N_("Filtro 2D"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.filter = f.render.filter; }},
        {"filter_strength", N_("Intensidad del filtro"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.render.filterStrength = f.render.filterStrength; }},
        {"upscaling", N_("Reescalado (experimental)"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.render.upscale = f.render.upscale; }},
        {"square_pixels", N_("Píxeles cuadrados"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.render.squarePixels = f.render.squarePixels; }},
        {"no_sprite_limit", N_("Sin límite de sprites"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.unlimitedSprites = f.unlimitedSprites; }},
        {"widescreen", N_("Pantalla ancha"), N_("Vídeo"), [](const Settings& f, Settings& t) {
             t.wideLeft = f.wideLeft;
             t.wideRight = f.wideRight;
         }},
        {"profile_3d", N_("Todos los ajustes del 3D"), "3D", [](const Settings& f, Settings& t) { copy3D(f.render, t.render); }},
        {"volume", N_("Volumen"), N_("Sonido"), [](const Settings& f, Settings& t) { t.volume = f.volume; }},
        {"mute", N_("Silencio"), N_("Sonido"), [](const Settings& f, Settings& t) { t.mute = f.mute; }},
        {"region", N_("Región"), N_("Consola"), [](const Settings& f, Settings& t) { t.region = f.region; }},
        {"pads", N_("Mando 2"), N_("Consola"), [](const Settings& f, Settings& t) { t.padType = f.padType; }},
        {"overclock", N_("Overclock"), N_("Consola"), [](const Settings& f, Settings& t) { t.overclock = f.overclock; }},
        {"rewind_seconds", N_("Segundos de rebobinado"), N_("Velocidad"),
         [](const Settings& f, Settings& t) { t.rewindSeconds = f.rewindSeconds; }},
        {"fast_forward", N_("Avance rápido"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.fastForward = f.fastForward; }},
        {"slow_motion", N_("Cámara lenta"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.slowMotion = f.slowMotion; }},
        {"run_ahead", N_("Run-ahead (menos latencia)"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.runAhead = f.runAhead; }},
    };
    return options;
}

const GameOption* findGameOption(const std::string& key) {
    for (const GameOption& o : gameOptions())
        if (key == o.key) return &o;
    return nullptr;
}

void GameOverrides::applyTo(Settings& to) const {
    for (const GameOption& o : gameOptions())
        if (has(o.key)) o.copy(values, to);
}

void GameOverrides::restore(const Settings& general, Settings& to) const {
    for (const GameOption& o : gameOptions())
        if (has(o.key)) o.copy(general, to);
}

GameOverrides parseGameOverrides(const std::string& text, const Settings& general, const InputConfig& generalInput) {
    GameOverrides o;
    o.values = general;
    o.controls = generalInput;
    std::istringstream controls(text);
    if (InputConfig::parse(controls, o.controls)) o.own.insert(kGameControls);
    if (parseProfile(text, o.values.render)) o.own.insert("profile_3d");
    // Settings::parse reads the values; which options the file has comes from its keys.
    std::istringstream keys(text);
    std::string line, section;
    while (std::getline(keys, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            section = iniName(lower(trim(line.substr(1, line.size() - 2))));
            continue;
        }
        const size_t eq = line.find('=');
        // The controls' sections share key names (fast_forward...) with the options.
        if (eq == std::string::npos || section == "profile_3d" || section == "keyboard" || section == "keyboard2" ||
            section == "gamepad")
            continue;
        const std::string key = optionFor(lower(trim(line.substr(0, eq))));
        if (key != "profile_3d" && findGameOption(key)) o.own.insert(key);
    }
    Settings read = o.values;
    std::istringstream in(text);
    Settings::parse(in, read);
    for (const GameOption& opt : gameOptions())
        if (o.has(opt.key) && std::string(opt.key) != "profile_3d") opt.copy(read, o.values);
    return o;
}

std::string formatGameOverrides(const GameOverrides& o, const std::string& game) {
    if (o.own.empty()) return {};
    std::ostringstream out;
    out << "# plumnin options of " << game << ". While this game is loaded, these settings\n"
        << "# replace those in plumnin.ini.\n";
    if (o.has("profile_3d")) {
        out << "\n[profile_3d]\n";
        write3D(out, o.values.render);
    }
    if (o.has(kGameControls)) {
        out << "\n";
        o.controls.writeButtons(out);
    }
    // The rest as plumnin.ini writes them, keeping only this game's keys and their sections.
    std::ostringstream all;
    o.values.write(all);
    std::istringstream in(all.str());
    std::string line, header;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            header = t;
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = optionFor(lower(trim(t.substr(0, eq))));
        if (key == "profile_3d" || !o.has(key) || !findGameOption(key)) continue;
        if (!header.empty()) {
            out << "\n" << header << "\n";
            header.clear();
        }
        out << t << "\n";
    }
    return out.str();
}

}  // namespace plumnin
