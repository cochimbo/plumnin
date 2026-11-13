#include "ui.h"

#include "handheld.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

#include "imgui.h"
#include "imgui_internal.h"  // ImGui::TableSetColumnSortDirection
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "game_options.h"
#include "languages.h"
#include "library_names.h"
#include "theme.h"
#include "icon_png.h"
#include "logo_png.h"
#include "../core/console.h"
#include "../core/i18n.h"
#include "../core/system.h"
#include "../net/udp.h"
#include "../render/gl.h"

#define STBI_NO_STDIO
#include "stb_image.h"

#ifndef PLUMNIN_VERSION
#define PLUMNIN_VERSION "dev"
#endif

namespace plumnin {

namespace {

constexpr int kRows = InputConfig::kButtons + InputConfig::kHotkeys;
// While a game runs the mouse pointer goes away once it has been still this long.
constexpr Uint32 kCursorIdleMs = 2000;
constexpr const char* kRowNames[kRows] = {N_("Arriba"), N_("Abajo"), N_("Izquierda"), N_("Derecha"), "A", "B", "X", "Y",
                                          "L", "R", "Select", "Start", N_("Efecto 3D"), N_("Guardar estado"), N_("Cargar estado"),
                                          N_("Ranura anterior"), N_("Ranura siguiente"), N_("Menú de estados"),
                                          N_("Rebobinar (mantener)"), N_("Avance rápido (mantener)"), N_("Cámara lenta"),
                                          N_("Biblioteca"), N_("Parar emulación"), N_("Contador de FPS")};
static_assert(InputConfig::kButtons == 12 && InputConfig::kHotkeys == 12, "update kRowNames");

std::vector<SDL_Scancode>& keysOf(InputConfig& c, int row, int player) {
    if (row >= InputConfig::kButtons) return c.hotkeyKeys[row - InputConfig::kButtons];
    return player == 0 ? c.keys[row] : c.keys2[row];
}

std::vector<SDL_GameControllerButton>& padOf(InputConfig& c, int row) {
    return row < InputConfig::kButtons ? c.pad[row] : c.hotkeyPad[row - InputConfig::kButtons];
}

std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A code of several joined with '+' shown as its first and how many more ("AJ0T-CA9Y +5").
std::string shortCode(const std::string& code) {
    const size_t plus = code.find('+');
    if (plus == std::string::npos) return code;
    size_t more = 0;
    for (char c : code) more += c == '+';
    return code.substr(0, plus) + " +" + std::to_string(more);
}

// ImGui::Combo with its items translated.
// The widescreen's controls: a button per ready-made width and a slider per side. They
// change the picture at once; the return value says when to save.
bool widescreenControls(int& left, int& right) {
    bool changed = false;
    for (int i = 0; i < kWidescreenCount; ++i) {
        if (i) ImGui::SameLine();
        const Widescreen& w = kWidescreens[i];
        if (ImGui::RadioButton(w.name, left == w.columns && right == w.columns)) {
            left = right = w.columns;
            changed = true;
        }
    }
    ImGui::SliderInt(tr("Izquierda##ancha"), &left, 0, VideoFrame::kMaxWideSide, "%d px", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderInt(tr("Derecha##ancha"), &right, 0, VideoFrame::kMaxWideSide, "%d px", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    return changed;
}

// Back to the default look of the pseudo-3D (the effect itself stays on or off).
void reset3D(RenderOptions& r) {
    const RenderOptions defaults;
    r.style = defaults.style;
    r.depth = defaults.depth;
    r.tilt = defaults.tilt;
    r.yaw = defaults.yaw;
    r.zoom = defaults.zoom;
    r.extrusion = defaults.extrusion;
    r.thickness = defaults.thickness;
    r.shadows = defaults.shadows;
    r.tiltCamera = defaults.tiltCamera;
    r.lighting = defaults.lighting;
    r.liveCamera = defaults.liveCamera;
    r.highBInFront = defaults.highBInFront;
    r.flatB = defaults.flatB;
}

bool comboTr(const char* label, int* current, const char* const items[], int count) {
    std::vector<const char*> shown(items, items + count);
    for (const char*& item : shown) item = tr(item);
    return ImGui::Combo(label, current, shown.data(), count);
}

// The run-ahead frames, 0 (off) to Settings::kMaxRunAhead.
bool runAheadSlider(const char* label, int& frames) {
    const char* shown = frames == 0 ? tr("desactivado") : frames == 1 ? tr("1 fotograma") : tr("%d fotogramas");
    return ImGui::SliderInt(label, &frames, 0, Settings::kMaxRunAhead, shown, ImGuiSliderFlags_AlwaysClamp);
}

// The overclock's choices, as percent of the console's processor speed.
constexpr int kOverclocks[] = {100, 150, 200, 300, 400};
constexpr const char* kOverclockNames[] = {N_("Desactivado (velocidad real)"), "150 %", "200 %", "300 %", "400 %"};
constexpr const char* kOverclockTip =
    N_("Hace más rápido el procesador de la consola, para los juegos que se ralentizan cuando hay\n"
       "mucho en pantalla. Cuanto más alto, más pide al ordenador. Algún juego puede ir demasiado\n"
       "rápido o fallar: si pasa, desactívalo para ese juego. En una partida en red manda el del\n"
       "anfitrión, y en modo hardcore se queda desactivado.");
constexpr const char* kNoPadTip =
    N_("Como si no hubiera nada enchufado en el segundo puerto. En una partida en red el mando 2\n"
       "se enchufa.");

// What can go in each port, and its name.
std::vector<PadType> portTypes(int port) {
    if (port == 0) return {PadType::Pad};
    return {PadType::Pad, PadType::None};
}
const char* padTypeName(PadType t) { return t == PadType::None ? N_("Sin mando") : N_("Mando"); }
const char* padTypeTip(PadType t) { return t == PadType::None ? kNoPadTip : nullptr; }

// The combo for the overclock; `percent` takes the chosen value.
// An on/off option as a "Sí" / "No" list, so a row of the game's options has no second
// box next to the one that says whether the game has its own value.
bool yesNo(const char* label, bool& value) {
    static const char* const names[] = {N_("No"), N_("Sí")};
    int i = value ? 1 : 0;
    if (!comboTr(label, &i, names, 2)) return false;
    value = i == 1;
    return true;
}

bool overclockCombo(const char* label, int& percent) {
    int i = 0;
    for (int k = 0; k < static_cast<int>(std::size(kOverclocks)); ++k)
        if (percent >= kOverclocks[k]) i = k;
    if (!comboTr(label, &i, kOverclockNames, static_cast<int>(std::size(kOverclocks)))) return false;
    percent = kOverclocks[i];
    return true;
}

// One line of the states menu's tabs of options: what it is, its value, and what left
// (-1), right (+1) and A or a double click (0) do to it.
struct MenuOption {
    std::string label;
    std::string value;
    bool enabled = true;
    std::function<void(int)> change;
    int effects = 0;  // what a change asks main() for, MenuEffect bits
    bool arrows = true;  // false for the ones that only do something with A, and have no value
};

enum MenuEffect {
    kSave = 1,          // write plumnin.ini
    kRewind = 8,        // the rewind length changed
    kFullscreen = 16,
};

const char* onOff(bool on) { return on ? tr("activado") : tr("desactivado"); }

std::string formatted(const char* format, double value) {
    char text[32];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

MenuOption toggle(const char* label, bool& value, int effects = kSave, bool enabled = true) {
    return {tr(label), onOff(value), enabled, [&value](int) { value = !value; }, effects};
}

// A number from `low` to `high` in steps of `step`, shown with the printf `format` (of a double).
MenuOption number(const char* label, int& value, int low, int high, int step, const char* format, int effects = kSave,
                  bool enabled = true) {
    return {tr(label), formatted(format, value), enabled,
            [&value, low, high, step](int dir) { value = std::clamp(value + (dir ? dir : 1) * step, low, high); },
            effects};
}

// The same for a float, 0..1 shown as a percentage unless `scale` says otherwise.
MenuOption amount(const char* label, float& value, float low, float high, float step, const char* format,
                  float scale = 100.0f, int effects = kSave, bool enabled = true) {
    return {tr(label), formatted(format, value * scale), enabled,
            [&value, low, high, step](int dir) {
                // Rounded to the step, so that the values shown stay round.
                const float v = std::round(value / step) * step + static_cast<float>(dir ? dir : 1) * step;
                value = std::clamp(v, low, high);
            },
            effects};
}

// One of `count` choices `names` (translated), going round: `current` is the one now,
// and `set` takes the new one.
MenuOption choice(const char* label, int current, const char* const names[], int count, std::function<void(int)> set,
                  int effects = kSave, bool enabled = true) {
    current = std::clamp(current, 0, count - 1);
    return {tr(label), tr(names[current]), enabled,
            [current, count, set = std::move(set)](int dir) { set((current + (dir < 0 ? count - 1 : 1)) % count); },
            effects};
}

constexpr const char* kFpsCornerNames[] = {N_("Arriba a la izquierda"), N_("Arriba a la derecha"),
                                           N_("Abajo a la izquierda"), N_("Abajo a la derecha")};
static_assert(std::size(kFpsCornerNames) == Settings::kFpsCorners, "a name per corner");

// The interface's sizes to choose from, in percent (0 automatic).
constexpr int kUiScales[] = {0, 75, 100, 125, 150, 175, 200, 250};
constexpr int kUiScaleCount = static_cast<int>(std::size(kUiScales));
std::string uiScaleName(int i) {
    return kUiScales[i] ? std::to_string(kUiScales[i]) + " %" : std::string(tr("Automático"));
}
// Settings::consoleMode's: automatic (saying why it is on, if it is), on, off.
std::string consoleModeName(int mode) {
    if (mode == 1) return tr("Activado");
    if (mode == 2) return tr("Desactivado");
    const std::string& why = consoleModeReason();
    return why.empty() ? std::string(tr("Automático")) : trf("Automático (%s)", why.c_str());
}

constexpr const char* kRegionNames[] = {N_("Automática (la de la ROM)"), N_("Japón (NTSC)"), N_("EE. UU. (NTSC)"),
                                        N_("Europa (PAL)")};

// What a code writes, one line per write, for its tooltip.
std::string describePatches(const std::vector<CheatPatch>& patches) {
    std::string out;
    char line[64];
    for (const CheatPatch& p : patches) {
        std::snprintf(line, sizeof line, "%s%s %06X = %0*X (%s)", out.empty() ? "" : "\n", p.ram ? "RAM" : "ROM",
                      static_cast<unsigned>(p.address), p.byte ? 2 : 4, static_cast<unsigned>(p.value),
                      p.byte ? tr("byte") : tr("palabra"));
        out += line;
    }
    return out;
}

}  // namespace

namespace {

bool fileExists(const char* path) {
    SDL_RWops* f = SDL_RWFromFile(path, "rb");
    if (f) SDL_RWclose(f);
    return f != nullptr;
}

}  // namespace

// The interface's font: Segoe UI on Windows (it reads better than the built-in bitmap
// font and covers the accents of every language plumnin speaks: Latin-1 and Latin
// Extended-A, ç, ã, œ...), with the characters of other scripts that the texts shown
// have needed (a Japanese name in a game's description, say) taken from the system's
// fonts for them. Only those characters go in the atlas, so it stays small.
void Ui::buildFonts() {
    fontsDirty_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontGlyphRangesBuilder builder;
    static const ImWchar base[] = {0x0020, 0x017F, 0x2010, 0x2026, 0};
    builder.AddRanges(base);
    for (ImWchar c : fontExtra_) builder.AddChar(c);
    fontRanges_.clear();
    builder.BuildRanges(&fontRanges_);
#ifdef _WIN32
    // Japanese, Chinese, Korean and symbols, in that order: the first one with a character gives it.
    const char* const fallbacks[] = {"C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc",
                                     "C:\\Windows\\Fonts\\msgothic.ttc", "C:\\Windows\\Fonts\\msyh.ttc",
                                     "C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\seguisym.ttf"};
#else
    const char* const fallbacks[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                     "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                     "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                     "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"};
#endif
    const float size = std::round(18.0f * scale_);
    // The theme's two fonts, each with the other scripts merged into it.
    auto merge = [&] {
        if (fontExtra_.empty()) return;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        for (const char* path : fallbacks)
            if (fileExists(path)) io.Fonts->AddFontFromFileTTF(path, size, &cfg, fontRanges_.Data);
    };
    theme::addBodyFont(size, fontRanges_.Data);
    merge();
    theme::addTitleFont(size, fontRanges_.Data);
    merge();
}

// Asks for the characters of `text` (UTF-8) that the font does not have yet; the atlas
// is made again with them before the next frame.
void Ui::needGlyphs(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        unsigned code = c;
        int extra = 0;
        if (c >= 0xF0) { code = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { code = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { code = c & 0x1F; extra = 1; }
        ++i;
        for (; extra > 0 && i < text.size(); --extra, ++i) code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        // Beyond the base ranges and within what ImWchar holds.
        if (code <= 0x17F || (code >= 0x2010 && code <= 0x2026) || code > 0xFFFF) continue;
        if (fontExtra_.insert(static_cast<ImWchar>(code)).second) fontsDirty_ = true;
    }
}

Ui::Ui(SDL_Window* window, SDL_GLContext context, Settings& settings, Input& input)
    : window_(window), settings_(settings), input_(input) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is not worth a second settings file
    scale_ = wantedScale();
    theme::apply(scale_);
    buildFonts();
    ImGui_ImplSDL2_InitForOpenGL(window_, context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    loadLogo();
    applyScraperSettings();
}

Ui::~Ui() {
    library_.clearTextures();
    gameInfo_.clearTextures();
    scraper_.clearTextures();
    if (logoTexture_) gl::DeleteTextures(1, &logoTexture_);
    if (iconTexture_) gl::DeleteTextures(1, &iconTexture_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

// An RGBA texture from an embedded PNG; 0 if it cannot be decoded. With `window`, the
// image is also the window's icon.
static unsigned int textureFromPng(const unsigned char* png, size_t size, int* w, int* h, SDL_Window* window = nullptr) {
    int n = 0;
    stbi_uc* rgba = stbi_load_from_memory(png, static_cast<int>(size), w, h, &n, 4);
    if (!rgba) return 0;
    unsigned int texture = 0;
    gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, *w, *h, 0, gl::RGBA, gl::UNSIGNED_BYTE, rgba);
    if (window) {
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(rgba, *w, *h, 32, *w * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(window, s);
            SDL_FreeSurface(s);
        }
    }
    stbi_image_free(rgba);
    return texture;
}

void Ui::loadLogo() {
    logoTexture_ = textureFromPng(kLogoPng, sizeof(kLogoPng), &logoWidth_, &logoHeight_);
    // The plum is also the window's icon (on Windows the .exe's icon already is).
    int w = 0, h = 0;
    iconTexture_ = textureFromPng(kIconPng, sizeof(kIconPng), &w, &h, window_);
}

bool Ui::handleEvent(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL)
        lastMouseMove_ = SDL_GetTicks();
    if (capture_.active) {
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) capture_.active = false;
            else if (capture_.column < 2) finishCapture(e.key.keysym.scancode);
            return true;
        }
        if (e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT) return true;
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (capture_.column == 2) finishCapture(e.cbutton.button);
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONUP) return true;
        if (e.type == SDL_CONTROLLERAXISMOTION) {
            const SDL_GameControllerButton trigger = triggerPulled(e);
            if (capture_.column == 2 && trigger != SDL_CONTROLLER_BUTTON_INVALID) finishCapture(trigger);
            return true;
        }
    }
    if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT) typedOnKeys_ = true;
    else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_FINGERDOWN) typedOnKeys_ = false;
    if (osk_.isOpen() && osk_.handleEvent(e)) return true;
    if (browser_.isOpen() && browser_.handleEvent(e)) return true;
    if (stateMenu_.open && handleStateMenu(e)) return true;
    if (showLibrary_ && handleLibrary(e)) return true;
    // With no game running and no window taking the pad, the empty screen has nothing for
    // it: A, B, Start or Back open the library again, so a pad alone never gets stuck there.
    if (!showLibrary_ && !romLoaded_ && !padNav_ && !stateMenu_.open && !browser_.isOpen() && !osk_.isOpen() &&
        e.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B:
            case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_BACK:
                toggleLibrary();
                return true;
            default: break;
        }
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
    // The pad on a window (console mode): ImGui reads it by itself, and its presses are not
    // the game's nor hotkeys, but for the states menu's.
    if (padNav_ && (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION))
        return input_.hotkeyFor(e) != Hotkey::StateMenu;
    return false;
}

void Ui::toggleStateMenu(int tab) {
    stateMenu_.open = !stateMenu_.open;
    if (tab >= 0 && tab < StateMenu::kTabs) stateMenu_.tab = tab;
    stateMenu_.selected = settings_.stateSlot;
    stateMenu_.scroll = true;
    stateMenu_.changes.clear();
    stateMenu_.held = 0;
    stateMenu_.tabChanged = true;  // it opens on the tab it was left on
}

// While the states menu is open it owns the pad and the keyboard: L1 and R1 (or Q and E)
// change tab. On the slots, up and down pick a slot, A (or Enter) loads it, Y (or Space)
// saves to it; on the tabs of options, up and down pick an option, left and right change
// it and A (or Enter) switches it. B (or Esc) closes the menu.
bool Ui::handleStateMenu(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_KEYUP &&
        e.type != SDL_CONTROLLERBUTTONUP && e.type != SDL_TEXTINPUT)
        return false;
    if (e.type == SDL_CONTROLLERBUTTONUP) {
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
            stateMenu_.held = 0;
        return true;
    }
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN) return true;
    if (input_.hotkeyFor(e) == Hotkey::StateMenu) {
        stateMenu_.open = false;
        return true;
    }
    enum { None, Up, Down, Left, Right, Load, Save, Close, PrevTab, NextTab } action = None;
    if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_UP: action = Up; break;
            case SDL_SCANCODE_DOWN: action = Down; break;
            case SDL_SCANCODE_LEFT: action = Left; break;
            case SDL_SCANCODE_RIGHT: action = Right; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: action = Load; break;
            case SDL_SCANCODE_SPACE: action = Save; break;
            case SDL_SCANCODE_ESCAPE: action = Close; break;
            case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: action = PrevTab; break;
            case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: action = NextTab; break;
            default: break;
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: action = Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = Down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: action = Left; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: action = Right; break;
            case SDL_CONTROLLER_BUTTON_A: action = Load; break;
            case SDL_CONTROLLER_BUTTON_Y: action = Save; break;
            case SDL_CONTROLLER_BUTTON_B: action = Close; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: action = PrevTab; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: action = NextTab; break;
            default: break;
        }
        // The pad does not repeat a held button as the keyboard does: drawStateMenu() does it.
        if (action == Left || action == Right) {
            stateMenu_.held = action == Left ? -1 : 1;
            stateMenu_.heldSince = SDL_GetTicks();
            stateMenu_.repeatAt = stateMenu_.heldSince + 400;
        }
    }
    constexpr int kTabs = StateMenu::kTabs;
    if (action == PrevTab || action == NextTab) {
        stateMenu_.tab = (stateMenu_.tab + (action == PrevTab ? kTabs - 1 : 1)) % kTabs;
        stateMenu_.tabChanged = true;
        stateMenu_.option = 0;
        stateMenu_.changes.clear();
        stateMenu_.scroll = true;
        return true;
    }
    if (action == Close) {
        stateMenu_.open = false;
        return true;
    }
    if (stateMenu_.tab != 0) {
        const int count = std::max(1, stateMenu_.options);
        switch (action) {
            case Up: stateMenu_.option = (stateMenu_.option + count - 1) % count; stateMenu_.scroll = true; break;
            case Down: stateMenu_.option = (stateMenu_.option + 1) % count; stateMenu_.scroll = true; break;
            case Left: stateMenu_.changes.push_back(-1); break;
            case Right: stateMenu_.changes.push_back(1); break;
            case Load: stateMenu_.changes.push_back(0); break;
            default: break;
        }
        return true;
    }
    const int count = std::max(1, stateMenu_.slots);
    switch (action) {
        case Up: stateMenu_.selected = (stateMenu_.selected + count - 1) % count; stateMenu_.scroll = true; break;
        case Down: stateMenu_.selected = (stateMenu_.selected + 1) % count; stateMenu_.scroll = true; break;
        case Load: stateMenu_.load = stateMenu_.selected; stateMenu_.open = false; break;
        case Save: stateMenu_.save = stateMenu_.selected; stateMenu_.open = false; break;
        default: break;
    }
    if (action == Load || action == Save) {
        settings_.stateSlot = stateMenu_.selected;
        pendingSave_ = true;
    }
    return true;
}

bool Ui::keyboardBusy() const {
    return capture_.active || stateMenu_.open || showLibrary_ || browser_.isOpen() || !error_.empty() ||
           ImGui::GetIO().WantCaptureKeyboard;
}

bool Ui::mouseBusy() const { return ImGui::GetIO().WantCaptureMouse; }

void Ui::finishCapture(int code) {
    const bool game = capture_.game && gameControls_;
    InputConfig c = game ? *gameControls_ : input_.config();
    if (capture_.column == 2) {
        auto& list = padOf(c, capture_.row);
        const auto b = static_cast<SDL_GameControllerButton>(code);
        if (!capture_.append) list.clear();
        if (std::find(list.begin(), list.end(), b) == list.end()) list.push_back(b);
    } else {
        auto& list = keysOf(c, capture_.row, capture_.column);
        const auto k = static_cast<SDL_Scancode>(code);
        if (!capture_.append) list.clear();
        if (std::find(list.begin(), list.end(), k) == list.end()) list.push_back(k);
    }
    capture_.active = false;
    if (game) {
        *gameControls_ = std::move(c);
        pendingGameOptions_ = true;
    } else {
        input_.setConfig(std::move(c));
        pendingSave_ = true;
    }
}

void Ui::setAchievements(Achievements* achievements) {
    achievements_ = achievements ? std::make_unique<AchievementsUi>(settings_, *achievements) : nullptr;
}

void Ui::notify(std::string message) {
    toast_ = std::move(message);
    toastUntil_ = SDL_GetTicks() + 2500;
}

bool Ui::consoleMode() const {
    if (consoleForced_ >= 0) return consoleForced_ == 1;
    if (settings_.consoleMode != 0) return settings_.consoleMode == 1;
    return !consoleModeReason().empty();
}

float Ui::wantedScale() const {
    if (settings_.uiScale > 0) return static_cast<float>(settings_.uiScale) / 100.0f;
    if (!consoleMode()) return 1.0f;
    // From the sofa or in the hand the letters have to be bigger: after the screen's
    // height, in quarters (1280x800 125 %, 1080p 175 %).
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return std::clamp(std::round(static_cast<float>(h) / 640.0f * 4.0f) / 4.0f, 1.0f, 2.5f);
}

void Ui::prepareFrame() {
    const float scale = wantedScale();
    if (scale != scale_) {
        scale_ = scale;
        theme::apply(scale_);
        fontsDirty_ = true;
    }
    // In console mode the pad drives ImGui's windows (the d-pad moves, A presses, B goes
    // back, X the menu bar) while one of them has the focus; the library, the states menu
    // and the file browser drive themselves. The game waits meanwhile (padInWindows()).
    // B with nothing left to close takes the focus away, and the pad is the game's again.
    ImGuiIO& io = ImGui::GetIO();
    const bool console = consoleMode();
    io.ConfigNavEscapeClearFocusWindow = console;
    // A text box being typed in without a keyboard (with the pad, a touch or the Steam
    // Deck's trackpads): the on-screen keyboard, which takes the pad meanwhile.
    if (console && io.WantTextInput && !typedOnKeys_) osk_.open();
    else osk_.close();
    // A window counts while it has the focus and takes the pad: not the overlays nor the
    // states menu (NoNav), and the menu bar only with the pad on its menus (ImGui may hand it
    // the focus when another window closes).
    const ImGuiContext& g = *GImGui;
    const ImGuiWindow* w = g.NavWindow;
    bool focused = w && w->WasActive && !(w->RootWindow->Flags & ImGuiWindowFlags_NoNav);
    if (focused && (w->RootWindow->Flags & ImGuiWindowFlags_MenuBar) && (w->RootWindow->Flags & ImGuiWindowFlags_NoTitleBar))
        focused = g.NavLayer == ImGuiNavLayer_Menu || g.OpenPopupStack.Size > 0;
    padNav_ = console && !osk_.isOpen() && !stateMenu_.open && !showLibrary_ && !browser_.isOpen() && !capture_.active &&
              (focusMenuBar_ || focused);
    if (menuForPad_ && !padNav_ && !focusMenuBar_) {
        settings_.showMenu = false;
        menuForPad_ = false;
    }
    if (padNav_) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

UiActions Ui::frame(const UiState& state) {
    UiActions out;
    romLoaded_ = state.romLoaded;
    out.settingsChanged = pendingSave_;
    pendingSave_ = false;
    out.gameOptionsChanged = pendingGameOptions_;
    pendingGameOptions_ = false;

    if (openRequested_) {
        openRequested_ = false;
        std::string path;
        // In full screen the system dialog can open behind the game's window and never
        // be seen, so there the built-in browser is used instead.
        const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        browseFor_ = BrowseFor::Game;
        switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, settings_.lastFolder, Pick::Game, &path)) {
            case DialogResult::Chosen: out.load = path; break;
            case DialogResult::Cancelled: break;
            case DialogResult::Unavailable: browser_.open(settings_.lastFolder); break;
        }
    }

    prepareFrame();
    if (fontsDirty_) {
        // Characters that came in a game's description, or a new size: the atlas is made again.
        buildFonts();
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    drawMenuBar(state, out);
    if (showControls_) drawControls(out);
    if (showVideo_) drawVideo(state, out);
    if (showAbout_) drawAbout();
    if (showCheats_) drawCheats(state, out);
    if (showGameOptions_) drawGameOptions(state, out);
    // With no game running, the covers can take more of each frame.
    library_.update(state.romLoaded && !state.paused ? 3 : 10);
    gameInfo_.update();
    scraper_.update();
    // A round of downloads that stops on an error says so wherever the user is.
    if (const bool failed = scraper_.batchFailed(); failed != scraperFailed_) {
        scraperFailed_ = failed;
        if (failed) notify(scraper_.batchStatus());
    }
    for (ScreenScraper::ForLibrary f; scraper_.takeForLibrary(f);) {
        library_.useDownloaded(f.path, f.cover, f.label, f.replace);
        gameInfo_.refresh(f.path);  // its description and screenshot, if it is in the panel
    }
    if (showLibrary_) drawLibrary(out);
    if (showMedia_) drawMedia(out);
    if (showScraping_) drawScraping(out);
    if (showNetplay_) drawNetplay(state, out);
    if (achievements_ && achievements_->drawWindows()) out.achievementsChanged = out.settingsChanged = true;
    stateMenu_.slots = static_cast<int>(state.slots.size());
    if (stateMenu_.load >= 0) out.loadState = stateMenu_.load;
    if (stateMenu_.save >= 0) out.saveState = stateMenu_.save;
    stateMenu_.load = stateMenu_.save = -1;
    if (stateMenu_.open) drawStateMenu(state, out);
    if (!error_.empty()) drawError();
    std::string picked;
    if (browser_.draw(&picked)) {
        switch (browseFor_) {
            case BrowseFor::Game: out.load = picked; break;
            case BrowseFor::LibraryFolder:
                if (std::find(settings_.libraryFolders.begin(), settings_.libraryFolders.end(), picked) ==
                    settings_.libraryFolders.end())
                    settings_.libraryFolders.push_back(picked);
                out.settingsChanged = true;
                rescanLibrary();
                break;
            case BrowseFor::CoverImage: useCoverImage(picked); break;
            case BrowseFor::Soundfont:
                settings_.soundfont = picked;
                out.settingsChanged = true;
                break;
        }
    }
    osk_.draw();
    drawOverlays(state);
    hideIdleCursor(state);
    return out;
}

// The backend reads the cursor ImGui asks for at the start of the next frame and hides
// the system pointer when it is None.
void Ui::hideIdleCursor(const UiState& state) {
    const bool windowOpen = showControls_ || showVideo_ || showAbout_ || showCheats_ || showGameOptions_ || showLibrary_ ||
                            showScraping_ || showNetplay_ || stateMenu_.open || browser_.isOpen() ||
                            (achievements_ && achievements_->windowOpen()) ||
                            !error_.empty() || capture_.active ||
                            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetIO().WantCaptureMouse;
    const bool playing = state.romLoaded && !state.paused;
    if (playing && !windowOpen && SDL_GetTicks() - lastMouseMove_ >= kCursorIdleMs)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void Ui::render() {
    theme::decorateWindows();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Ui::drawMenuBar(const UiState& state, UiActions& out) {
    const ImGuiIO& io = ImGui::GetIO();
    const float barHeight = ImGui::GetFrameHeight();
    if (!settings_.showMenu) {
        // Hidden bar: reveal it while the mouse sits at the top edge or a menu is open.
        const bool atTop = io.MousePos.y >= 0 && io.MousePos.y < (peekMenu_ ? barHeight : 4.0f);
        peekMenu_ = atTop || (peekMenu_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    }
    menuHeight_ = settings_.showMenu ? static_cast<int>(barHeight) : 0;
    if (!settings_.showMenu && !peekMenu_) return;
    if (!ImGui::BeginMainMenuBar()) return;

    if (focusMenuBar_) {
        // Asked from the states menu with the pad: the pad on the first menu.
        focusMenuBar_ = false;
        ImGuiWindow* bar = ImGui::GetCurrentWindow();
        ImGui::FocusWindow(bar);
        ImGui::SetFocusID(ImGui::GetID(tr("Archivo")), bar);
        GImGui->NavCursorVisible = GImGui->NavHighlightItemUnderNav = true;
    }
    if (ImGui::BeginMenu(tr("Archivo"))) {
        if (ImGui::MenuItem(tr("Abrir ROM..."), "Ctrl+O")) openRequested_ = true;
        if (ImGui::MenuItem(tr("Biblioteca..."), "F10", showLibrary_)) toggleLibrary();
        if (ImGui::BeginMenu(tr("Recientes"), !settings_.recent.empty())) {
            for (const auto& path : settings_.recent) {
                if (ImGui::MenuItem(fileName(path).c_str())) out.load = path;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Borrar lista"))) {
                settings_.recent.clear();
                out.settingsChanged = true;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Salir"))) out.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Juego"))) {
        // In a netplay game these would make the two consoles go different.
        const bool solo = state.romLoaded && !state.netPlaying;
        if (ImGui::MenuItem(tr("Pausa"), "P", state.paused, solo)) out.togglePause = true;
        if (ImGui::MenuItem(tr("Reiniciar"), "F1", false, solo)) out.reset = true;
        if (ImGui::MenuItem(tr("Parar emulación"), "F12", false, state.romLoaded)) out.stop = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Cierra el juego (guarda su partida de cartucho) y vuelve a la\n"
                              "pantalla de inicio, o a la biblioteca si tienes carpetas en ella."));
        if (ImGui::MenuItem(tr("Trucos..."), nullptr, false, solo && !state.hardcore)) {
            out.cheatsFor = state.romPath;
            showCheats_ = true;
        }
        if (ImGui::MenuItem(tr("Opciones de este juego..."), nullptr, false, solo)) {
            out.optionsFor = state.romPath;
            showGameOptions_ = true;
        }
        ImGui::SeparatorText(tr("Grabar"));
        if (ImGui::MenuItem(tr("Grabar vídeo"), "Ctrl+R", state.recording == 2, state.romLoaded)) out.record = 2;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Graba la partida con su sonido en un vídeo AVI, que abren Windows, VLC\n"
                              "y los editores de vídeo. Vuelve a pulsarlo para parar."));
        if (ImGui::MenuItem(tr("Grabar GIF"), "Ctrl+G", state.recording == 1, state.romLoaded)) out.record = 1;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Graba la partida, sin sonido, en un GIF animado para compartirlo.\n"
                              "Vuelve a pulsarlo para parar."));
        if (ImGui::MenuItem(tr("Abrir la carpeta de grabaciones"))) out.openRecordings = true;
        ImGui::SeparatorText(tr("Velocidad"));
        if (ImGui::MenuItem(tr("Cámara lenta"), "F9", state.slowMotion, solo && !state.hardcore)) out.toggleSlowMotion = true;
        if (state.hardcore) ImGui::TextDisabled("%s", tr("Modo hardcore: sin trucos, cámara lenta ni rebobinado"));
        ImGui::TextDisabled("%s", tr("Rebobinar: mantén Retroceso (o el gatillo izquierdo)"));
        ImGui::TextDisabled("%s", tr("Avance rápido: mantén Tab (o el gatillo derecho)"));
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt(tr("Segundos de rebobinado"), &settings_.rewindSeconds, 0, Settings::kMaxRewindSeconds,
                         settings_.rewindSeconds ? "%d s" : tr("desactivado"), ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = out.rewindChanged = true;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt(tr("Avance rápido"), &settings_.fastForward, 2, 10, "%dx", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt(tr("Cámara lenta##velocidad"), &settings_.slowMotion, 10, 90, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        if (ImGui::MenuItem(tr("Contador de FPS"), "F", settings_.fpsCounter)) {
            settings_.fpsCounter = !settings_.fpsCounter;
            out.settingsChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Los fotogramas por segundo del juego y lo que tarda cada uno en emularse,\n"
                              "con una gráfica de los últimos 10 segundos."));
        if (ImGui::BeginMenu(tr("Posición del contador"), settings_.fpsCounter)) {
            for (int c = 0; c < Settings::kFpsCorners; ++c)
                if (ImGui::MenuItem(tr(kFpsCornerNames[c]), nullptr, settings_.fpsCorner == c)) {
                    settings_.fpsCorner = c;
                    out.settingsChanged = true;
                }
            ImGui::EndMenu();
        }
        ImGui::SeparatorText(tr("Latencia"));
        ImGui::SetNextItemWidth(160);
        runAheadSlider(tr("Run-ahead"), settings_.runAhead);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Quita fotogramas de retardo entre pulsar un botón y verlo en pantalla: en cada\n"
                              "fotograma el juego corre ese número de fotogramas por delante, enseña su imagen\n"
                              "y vuelve atrás. Con 1 suele bastar; más piden más al ordenador y pueden notarse saltos."));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Estado"))) {
        if (ImGui::MenuItem(trf("Guardar estado (ranura %d)", settings_.stateSlot).c_str(), "F5", false, state.romLoaded))
            out.saveState = settings_.stateSlot;
        const bool used = settings_.stateSlot < static_cast<int>(state.slots.size()) &&
                          state.slots[static_cast<size_t>(settings_.stateSlot)].used;
        if (ImGui::MenuItem(trf("Cargar estado (ranura %d)", settings_.stateSlot).c_str(), "F8", false,
                            state.romLoaded && used && !state.netPlaying && !state.hardcore))
            out.loadState = settings_.stateSlot;
        if (ImGui::MenuItem(tr("Menú de estados..."), "F4")) toggleStateMenu();
        ImGui::SeparatorText(tr("Ranura (F6 / F7)"));
        for (int i = 0; i < static_cast<int>(state.slots.size()); ++i) {
            const SlotInfo& info = state.slots[static_cast<size_t>(i)];
            const std::string label = std::to_string(i) + "  " + (info.used ? info.when : std::string(tr("vacía")));
            if (ImGui::MenuItem(label.c_str(), nullptr, settings_.stateSlot == i)) {
                settings_.stateSlot = i;
                out.settingsChanged = true;
            }
            if (info.texture && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Image(static_cast<ImTextureID>(info.texture),
                             ImVec2(static_cast<float>(info.width) * 1.5f, static_cast<float>(info.height) * 1.5f));
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Consola"))) {
        ImGui::BeginDisabled(state.netPlaying);  // the host's console decides in a netplay game
        ImGui::SeparatorText(tr("Región"));
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(tr(kRegionNames[i]), nullptr, static_cast<int>(settings_.region) == i)) {
                settings_.region = static_cast<Region>(i);
                out.consoleChanged = out.settingsChanged = true;
            }
        for (int port = 1; port < 2; ++port) {
            ImGui::SeparatorText(port == 0 ? tr("Mando 1") : tr("Mando 2"));
            ImGui::PushID(port);
            for (PadType t : portTypes(port)) {
                if (ImGui::MenuItem(tr(padTypeName(t)), nullptr, settings_.padType[static_cast<size_t>(port)] == t)) {
                    settings_.padType[static_cast<size_t>(port)] = t;
                    out.consoleChanged = out.settingsChanged = true;
                }
                if (padTypeTip(t) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tr(padTypeTip(t)));
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::SeparatorText(tr("Overclock"));
        ImGui::BeginDisabled(state.netPlaying || state.hardcore);
        for (int k = 0; k < static_cast<int>(std::size(kOverclocks)); ++k) {
            if (ImGui::MenuItem(tr(kOverclockNames[k]), nullptr, settings_.overclock == kOverclocks[k])) {
                settings_.overclock = kOverclocks[k];
                out.settingsChanged = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(kOverclockTip));
        }
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Scraping"))) {
        if (ImGui::MenuItem(tr("Cuenta de ScreenScraper..."))) openScraping(0);
        if (ImGui::MenuItem(tr("Qué descargar..."))) openScraping(1);
        ImGui::Separator();
        // Every game of the library: what it is missing, or all of it again.
        const bool running = scraper_.batchLeft() > 0;
        if (running) {
            if (ImGui::MenuItem(tr("Parar la descarga"))) scraper_.cancelAll();
        } else {
            // Only what is missing (quick, from what was found before), or all of it again.
            bool all = false;
            bool go = ImGui::MenuItem(tr("Descargar lo que falte en toda la biblioteca"), nullptr, false, !settings_.libraryFolders.empty());
            if (ImGui::MenuItem(tr("Volver a descargar todo en toda la biblioteca"), nullptr, false, !settings_.libraryFolders.empty()))
                go = all = true;
            if (go) {
                showLibrary_ = true;
                if (!library_.scanned() || library_.folders() != settings_.libraryFolders) rescanLibrary();
                scrapeLibrary(all ? 1 : 0);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Red"))) {
        if (ImGui::MenuItem(tr("Jugar en red..."), nullptr, showNetplay_)) showNetplay_ = true;
        if (ImGui::MenuItem(tr("Salir de la partida en red"), nullptr, false, state.netSession)) out.netLeave = true;
        ImGui::EndMenu();
    }
    if (achievements_ && ImGui::BeginMenu(tr("Logros"))) {
        if (achievements_->drawMenu(state.romLoaded)) out.achievementsChanged = out.settingsChanged = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Sonido"))) {
        drawSoundMenu(out);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Opciones"))) {
        if (ImGui::MenuItem(tr("Controles..."))) showControls_ = true;
        if (ImGui::MenuItem(tr("Vídeo..."))) showVideo_ = true;
        if (achievements_ && ImGui::MenuItem("RetroAchievements...")) achievements_->openAccount();
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Efecto 3D"), nullptr, settings_.render.pseudo3d)) {
            settings_.render.pseudo3d = !settings_.render.pseudo3d;
            out.settingsChanged = true;
        }
        if (ImGui::MenuItem(tr("Pantalla completa"), "F11", settings_.fullscreen)) {
            settings_.fullscreen = !settings_.fullscreen;
            out.fullscreenChanged = out.settingsChanged = true;
        }
        if (ImGui::MenuItem(tr("Barra de menús"), "Esc", settings_.showMenu)) {
            settings_.showMenu = !settings_.showMenu;
            out.settingsChanged = true;
        }
        if (ImGui::BeginMenu(tr("Tamaño de la interfaz"))) {
            for (int i = 0; i < kUiScaleCount; ++i)
                if (ImGui::MenuItem(uiScaleName(i).c_str(), nullptr, settings_.uiScale == kUiScales[i])) {
                    settings_.uiScale = kUiScales[i];
                    out.settingsChanged = true;
                }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Modo consola"))) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(consoleModeName(i).c_str(), nullptr, settings_.consoleMode == i)) {
                    settings_.consoleMode = i;
                    out.settingsChanged = true;
                }
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Para jugar con mando desde el sofá o en una portátil\n"
                                         "(Steam Deck, ROG Ally, Legion Go, MSI Claw...): letra\n"
                                         "más grande y el mando en todas las ventanas."));
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // "Language" too, for whoever cannot read the language it is in.
        const std::string languageLabel =
            std::string(tr("Idioma")) + (language() == "en" ? "" : " (Language)") + "###idioma";
        if (ImGui::BeginMenu(languageLabel.c_str())) {
            for (const LanguageInfo& lang : kLanguages)
                if (ImGui::MenuItem(lang.name, nullptr, language() == lang.code)) {
                    settings_.language = lang.code;
                    applyLanguage(lang.code);
                    out.settingsChanged = true;
                }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Ayuda"))) {
        if (ImGui::MenuItem(tr("Acerca de plumnin"))) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Volume and mute, and the music: the chip's own or the synthesizer's.
void Ui::drawSoundMenu(UiActions& out) {
    if (ImGui::MenuItem(tr("Silenciar"), "M", settings_.mute)) {
        settings_.mute = !settings_.mute;
        out.settingsChanged = true;
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("Volumen"), &settings_.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Música"));
    if (ImGui::MenuItem(tr("Original"), nullptr, !settings_.synth)) {
        settings_.synth = false;
        out.settingsChanged = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("El sonido del chip de la consola, tal cual."));
    if (ImGui::MenuItem(tr("Sintetizador"), nullptr, settings_.synth)) {
        settings_.synth = true;
        out.settingsChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("La música de la Game Boy y la NES suena con instrumentos General MIDI del SoundFont\n"
                                   "elegido, como en un módulo SC-55: los pulsos como oboes, trompetas o clarinetes,\n"
                                   "el triángulo y los graves como bajo, la onda según su forma, y el ruido como batería.\n"
                                   "Las muestras de la NES (el DMC) siguen siendo las del juego. La Super Nintendo,\n"
                                   "de momento, suena siempre original."));
    ImGui::BeginDisabled(!settings_.synth);
    drawSoundfontCombo(out);
    if (ImGui::MenuItem(tr("Abrir la carpeta de SoundFonts"))) openInSystem(soundfontFolder_);
    if (ImGui::MenuItem(tr("Elegir otro (.sf2)..."))) chooseSoundfont(out);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("Sonido original"), &settings_.chipMix, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Cuánto se oye además la música original del chip."));
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("Reverb"), &settings_.reverb, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("Chorus"), &settings_.chorus, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::EndDisabled();
}

void Ui::setSoundfonts(std::string included, std::string folder) {
    includedSoundfont_ = std::move(included);
    soundfontFolder_ = std::move(folder);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(soundfontFolder_), ec);
}

// The SoundFonts to choose from, in a drop-down: the included one, and every .sf2 in
// the user's folder (read while it is open, so new downloads show up at once).
void Ui::drawSoundfontCombo(UiActions& out) {
    auto choose = [&](const std::string& path) {
        settings_.soundfont = path;
        out.settingsChanged = true;
    };
    auto name = [](const std::string& path) { return std::filesystem::u8path(path).stem().u8string(); };
    const std::string current =
        settings_.soundfont.empty() ? std::string(tr("GeneralUser GS (incluido)")) : name(settings_.soundfont);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##soundfont", current.c_str())) {
        if (ImGui::Selectable(tr("GeneralUser GS (incluido)"), settings_.soundfont.empty())) choose("");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", includedSoundfont_.c_str());
        std::vector<std::string> fonts;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(soundfontFolder_), ec)) {
            std::string ext = e.path().extension().u8string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (e.is_regular_file(ec) && ext == ".sf2") fonts.push_back(e.path().u8string());
        }
        std::sort(fonts.begin(), fonts.end());
        bool listed = settings_.soundfont.empty();
        for (const std::string& f : fonts) {
            const bool on = settings_.soundfont == f;
            listed = listed || on;
            if (ImGui::Selectable(name(f).c_str(), on)) choose(f);
        }
        if (!listed) ImGui::Selectable(current.c_str(), true);  // chosen from elsewhere
        if (fonts.empty()) ImGui::TextDisabled("%s", tr("Deja tus .sf2 en la carpeta de SoundFonts\ny aparecerán aquí."));
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("El SoundFont del sintetizador"));
}

void Ui::chooseSoundfont(UiActions& out) {
    const std::string start = settings_.soundfont.empty()
                                  ? settings_.lastFolder
                                  : settings_.soundfont.substr(0, settings_.soundfont.find_last_of("/\\"));
    std::string chosen;
    browseFor_ = BrowseFor::Soundfont;
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, start, Pick::AnyFile, &chosen)) {
        case DialogResult::Chosen:
            settings_.soundfont = chosen;
            out.settingsChanged = true;
            break;
        case DialogResult::Cancelled: break;
        case DialogResult::Unavailable: browser_.open(start, Pick::AnyFile); break;
    }
}

bool Ui::drawBindings(InputConfig& c, bool game) {
    bool changed = false;
    if (ImGui::BeginTable(game ? "game_bindings" : "bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Acción"), ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn(tr("Teclado (jugador 1)"));
        ImGui::TableSetupColumn(tr("Teclado (jugador 2)"));
        ImGui::TableSetupColumn(tr("Mando"));
        ImGui::TableHeadersRow();
        for (int row = 0; row < (game ? InputConfig::kButtons : kRows); ++row) {
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(kRowNames[row]));
            for (int col = 0; col < 3; ++col) {
                const bool pad = col == 2;
                ImGui::TableNextColumn();
                if (col == 1 && row >= InputConfig::kButtons) continue;  // hotkeys: one keyboard list
                ImGui::PushID(col);
                std::string label;
                if (capture_.active && capture_.game == game && capture_.row == row && capture_.column == col) {
                    label = pad ? tr("Pulsa un botón...") : tr("Pulsa una tecla...");
                } else if (pad) {
                    for (auto b : padOf(c, row)) label += (label.empty() ? "" : ", ") + std::string(padButtonName(b));
                } else {
                    for (auto k : keysOf(c, row, col)) label += (label.empty() ? "" : ", ") + std::string(SDL_GetScancodeName(k));
                }
                if (label.empty()) label = "-";
                const float small = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.x * 2;
                if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - small, 0)))
                    capture_ = {true, row, col, false, game};
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0))) capture_ = {true, row, col, true, game};
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
                    if (pad) padOf(c, row).clear();
                    else keysOf(c, row, col).clear();
                    capture_.active = false;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    return changed;
}

void Ui::drawControls(UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(theme::px(760), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(tr("Controles"), &showControls_)) {
        ImGui::End();
        return;
    }
    if (!showControls_ && !capture_.game) capture_.active = false;

    ImGui::TextWrapped("%s", tr("Pulsa una casilla y luego la tecla o el botón del mando. \"+\" añade otro, "
                                "\"x\" lo borra, Esc cancela."));
    if (input_.hasGameButtons())
        ImGui::TextDisabled("%s", tr("El juego abierto tiene sus propios botones (en Opciones de este juego); "
                                     "aquí cambias los generales."));
    InputConfig c = input_.config();
    bool changed = drawBindings(c, false);

    ImGui::Spacing();
    changed |= ImGui::Checkbox(tr("Usar el stick izquierdo como cruceta"), &c.stickAsDpad);
    ImGui::BeginDisabled(!c.stickAsDpad);
    int dead = c.stickDeadzone * 100 / 32767;
    if (ImGui::SliderInt(tr("Zona muerta del stick"), &dead, 0, 95, "%d%%")) {
        c.stickDeadzone = dead * 32767 / 100;
        changed = true;
    }
    ImGui::EndDisabled();

    const auto pads = input_.padNames();
    if (pads.empty()) {
        ImGui::TextDisabled("%s", tr("No hay mandos conectados."));
    } else {
        ImGui::Text("%s", tr("Mandos conectados:"));
        for (const auto& p : pads) ImGui::BulletText("%s", p.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button(tr("Restaurar predeterminados"))) {
        const InputConfig d = InputConfig::defaults();
        c.keys = d.keys;
        c.keys2 = d.keys2;
        c.pad = d.pad;
        c.hotkeyKeys = d.hotkeyKeys;
        c.hotkeyPad = d.hotkeyPad;
        c.stickAsDpad = d.stickAsDpad;
        c.stickDeadzone = d.stickDeadzone;
        capture_.active = false;
        changed = true;
    }
    if (changed) {
        input_.setConfig(std::move(c));
        out.settingsChanged = true;
    }
    ImGui::End();
}

void Ui::drawVideo(const UiState& state, UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(theme::px(420), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(tr("Vídeo"), &showVideo_)) {
        ImGui::End();
        return;
    }
    bool changed = false;

    if (ImGui::BeginCombo(tr("Vista"), settings_.view.c_str())) {
        for (const char* v : state.views) {
            if (ImGui::Selectable(v, settings_.view == v)) {
                settings_.view = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("2D: la imagen normal de la consola.\n"
                          "capas (depuración): pinta cada píxel según su capa (plano B azul, plano A\n"
                          "verde, ventana cian, sprites rojo, fondo gris; amarillento si tiene\n"
                          "prioridad alta); no son los colores del juego."));

    ImGui::SeparatorText(tr("Imagen"));
    int scale = static_cast<int>(settings_.render.scale);
    changed |= ImGui::RadioButton(tr("Escala entera"), &scale, static_cast<int>(ScaleMode::Integer));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Ajustar"), &scale, static_cast<int>(ScaleMode::Fit));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Estirar"), &scale, static_cast<int>(ScaleMode::Stretch));
    settings_.render.scale = static_cast<ScaleMode>(scale);
    changed |= ImGui::Checkbox(tr("Suavizar píxeles"), &settings_.render.smooth);
    changed |= ImGui::Checkbox(tr("Píxeles cuadrados (sin estirar a 4:3)"), &settings_.render.squarePixels);
    changed |= ImGui::Checkbox(tr("Sin límite de sprites (sin parpadeo)"), &settings_.unlimitedSprites);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("La consola solo dibuja 20 sprites (16 en modo estrecho) por línea; los juegos alternan cuáles\n"
                          "cada fotograma y por eso parpadean. Así se ven todos. El juego no lo nota, así que\n"
                          "no afecta a partidas en red, logros ni estados. Si un juego parpadea igual, es que\n"
                          "lo hace él mismo."));

    ImGui::SeparatorText(tr("Pantalla ancha"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Enseña a los lados lo que el juego tiene más allá de los bordes de la pantalla.\n"
                          "Cada lado se ajusta por separado y se ve al momento, con la imagen siempre centrada.\n"
                          "En muchos juegos se ve bien; en otros aparecen enemigos antes de tiempo, trozos\n"
                          "del nivel sin dibujar o cosas que se esconden fuera: entonces quita columnas de ese lado.\n"
                          "El juego no lo nota, así que no afecta a partidas en red, logros ni estados."));
    changed |= widescreenControls(settings_.wideLeft, settings_.wideRight);
    ImGui::BeginDisabled(!state.romLoaded);
    bool wideProfile = state.hasWideProfile;
    if (ImGui::Checkbox(tr("Guardar la pantalla ancha solo para este juego"), &wideProfile)) out.wideProfile = wideProfile ? 1 : 0;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", state.romLoaded ? trf("Los lados de arriba se guardan para %s y se recuperan al abrirlo.\n"
                                                      "Al desmarcarlo, el juego vuelve a usar los generales.",
                                                      state.gameName.c_str()).c_str()
                                                : tr("Abre una ROM para guardarle su propia pantalla ancha."));
    ImGui::Separator();
    const bool upscaled = settings_.render.upscale != Upscale2D::None;
    ImGui::BeginDisabled(upscaled);
    int filter = static_cast<int>(settings_.render.filter);
    ImGui::TextUnformatted(tr("Filtro 2D:"));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Ninguno"), &filter, static_cast<int>(Filter2D::None));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Scanlines"), &filter, static_cast<int>(Filter2D::Scanlines));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("CRT"), &filter, static_cast<int>(Filter2D::Crt));
    settings_.render.filter = static_cast<Filter2D>(filter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Scanlines: las líneas oscuras entre las líneas de la imagen, como en una tele.\n"
                          "CRT: además, la pantalla un poco curva, la máscara de colores y las esquinas\n"
                          "más oscuras. Solo en la vista 2D; se ve mejor a pantalla completa."));
    ImGui::BeginDisabled(settings_.render.filter == Filter2D::None);
    float strength = settings_.render.filterStrength * 100.0f;
    if (ImGui::SliderFloat(tr("Intensidad del filtro"), &strength, 0.0f, 100.0f, "%.0f %%"))
        settings_.render.filterStrength = strength / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::PushID("reescalado");
    int upscale = static_cast<int>(settings_.render.upscale);
    ImGui::TextUnformatted(tr("Reescalado (experimental):"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Dibuja la imagen 2D más grande de lo que la hace la consola, adivinando el detalle,\n"
                          "en la tarjeta gráfica y a 60 fps. Sustituye al filtro 2D mientras está puesto.\n"
                          "xBR: redondea los bordes en escalera de las diagonales; el pixel art se ve\n"
                          "como dibujado a mano.\n"
                          "Anime4K: una red neuronal pequeña que dobla la imagen una o dos veces según\n"
                          "el tamaño de la ventana. Más suave; pensada para dibujos animados."));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Ninguno"), &upscale, static_cast<int>(Upscale2D::None));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("xBR", &upscale, static_cast<int>(Upscale2D::Xbr));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Anime4K", &upscale, static_cast<int>(Upscale2D::Anime4K));
    settings_.render.upscale = static_cast<Upscale2D>(upscale);
    ImGui::PopID();

    ImGui::SeparatorText(tr("Ventana"));
    ImGui::BeginDisabled(settings_.fullscreen);
    ImGui::TextUnformatted(tr("Tamaño:"));
    for (int s = 1; s <= 6; ++s) {
        ImGui::SameLine();
        const std::string label = std::to_string(s) + "x";
        if (ImGui::Button(label.c_str())) {
            settings_.windowScale = s;
            out.windowScale = s;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox(tr("Pantalla completa (F11)"), &settings_.fullscreen)) {
        out.fullscreenChanged = true;
        changed = true;
    }
    changed |= ImGui::Checkbox(tr("Mostrar la barra de menús (Esc)"), &settings_.showMenu);
    {
        int scale = 0;
        for (int i = 0; i < kUiScaleCount; ++i)
            if (settings_.uiScale == kUiScales[i]) scale = i;
        const std::string current = uiScaleName(scale);
        if (ImGui::BeginCombo(tr("Tamaño de la interfaz"), current.c_str())) {
            for (int i = 0; i < kUiScaleCount; ++i)
                if (ImGui::Selectable(uiScaleName(i).c_str(), i == scale)) {
                    settings_.uiScale = kUiScales[i];
                    changed = true;
                }
            ImGui::EndCombo();
        }
        const std::string mode = consoleModeName(settings_.consoleMode);
        if (ImGui::BeginCombo(tr("Modo consola"), mode.c_str())) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(consoleModeName(i).c_str(), i == settings_.consoleMode)) {
                    settings_.consoleMode = i;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Para jugar con mando desde el sofá o en una portátil\n"
                                       "(Steam Deck, ROG Ally, Legion Go, MSI Claw...): letra\n"
                                       "más grande y el mando en todas las ventanas."));
    }

    ImGui::SeparatorText(tr("Pseudo-3D"));
    ImGui::BeginDisabled(!state.romLoaded);
    bool profile = state.hasProfile;
    if (ImGui::Checkbox(tr("Guardar el 3D solo para este juego"), &profile)) out.profile = profile ? 1 : 0;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", state.romLoaded ? trf("Los ajustes de abajo (también el efecto 3D y la cámara) se guardan\n"
                                                      "para %s y se recuperan al abrirlo.\n"
                                                      "Al desmarcarlo, el juego vuelve a usar los ajustes generales.",
                                                      state.gameName.c_str()).c_str()
                                                : tr("Abre una ROM para guardarle un perfil 3D propio."));
    if (state.hasProfile) {
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(perfil de %s)"), state.gameName.c_str());
    }
    changed |= ImGui::Checkbox(tr("Efecto 3D"), &settings_.render.pseudo3d);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("(atajo en Opciones > Controles)"));
    RenderOptions& r = settings_.render;
    ImGui::BeginDisabled(!r.pseudo3d);
    int style = static_cast<int>(r.style);
    ImGui::TextUnformatted(tr("Estilo:"));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Vóxeles"), &style, static_cast<int>(Style3D::Voxels));
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Capas"), &style, static_cast<int>(Style3D::Layers));
    r.style = static_cast<Style3D>(style);
    // Sliders save once released rather than on every frame of the drag.
    float depth = r.depth * 100.0f, thickness = r.thickness * 100.0f;
    if (ImGui::SliderFloat(tr("Profundidad"), &depth, 0.0f, 100.0f, "%.0f %%")) r.depth = depth / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::Checkbox(tr("Inclinar la cámara"), &r.tiltCamera);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Sin marcar, la cámara mira de frente y la imagen se ve como en 2D;\n"
                          "la profundidad se nota con la iluminación y la cámara viva."));
    ImGui::BeginDisabled(!r.tiltCamera);
    ImGui::SliderFloat(tr("Inclinación"), &r.tilt, kMinTilt, kMaxTilt, "%.0f°");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderFloat(tr("Giro"), &r.yaw, -kMaxYaw, kMaxYaw, "%.0f°");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    ImGui::SliderFloat(tr("Zoom"), &r.zoom, kMinZoom, kMaxZoom, "%.2fx");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SliderFloat(tr("Grosor"), &thickness, 0.0f, 100.0f, "%.0f %%")) r.thickness = thickness / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::BeginDisabled(r.style != Style3D::Voxels);
    float extrusion = r.extrusion * 100.0f;
    if (ImGui::SliderFloat(tr("Extrusión"), &extrusion, 0.0f, 100.0f, "%.0f %%")) r.extrusion = extrusion / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox(tr("Iluminación"), &r.lighting);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Luz lateral sobre los bloques y sombras suaves de cada plano sobre los de detrás.\n"
                          "Hace que el relieve se vea también con la cámara de frente."));
    changed |= ImGui::Checkbox(tr("Cámara viva"), &r.liveCamera);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("La cámara se balancea unos grados siguiendo la acción,\n"
                          "así las capas se mueven entre sí aunque mires de frente."));
    changed |= ImGui::Checkbox(tr("Sombra de los sprites"), &r.shadows);
    changed |= ImGui::Checkbox(tr("Plano B con prioridad delante"), &r.highBInFront);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Los tiles del plano B con prioridad alta se ponen delante, con los del plano A."));
    changed |= ImGui::Checkbox(tr("Plano B plano (como el cielo)"), &r.flatB);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Para juegos que pintan el cielo en el plano B: no lo levanta en 3D."));
    if (ImGui::Button(tr("Valores por defecto"))) {
        reset3D(r);
        changed = true;
    }
    ImGui::TextDisabled("%s", tr("Cámara: arrastra con el ratón o usa el stick derecho para girar,\n"
                        "la rueda para acercar y doble clic o el clic del stick para centrarla."));
    ImGui::EndDisabled();

    out.settingsChanged |= changed;
    ImGui::End();
}

void Ui::drawStateMenu(const UiState& state, UiActions& out) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - 20.0f, theme::px(520.0f)), std::min(vp->Size.y - 40.0f, theme::px(540.0f))),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin(tr("Estados"), &stateMenu_.open, flags)) {
        ImGui::End();
        return;
    }
    // The pad's left or right held down repeats, faster than the keyboard's.
    if (stateMenu_.held && stateMenu_.tab != 0 && SDL_GetTicks() >= stateMenu_.repeatAt) {
        stateMenu_.changes.push_back(stateMenu_.held);
        stateMenu_.repeatAt = SDL_GetTicks() + 70;
    }
    static const char* const kTabNames[StateMenu::kTabs] = {N_("Estados"), N_("Imagen"), "3D", N_("Sonido"),
                                                            N_("Velocidad"), N_("Sistema")};
    if (ImGui::BeginTabBar("pestañas")) {
        for (int t = 0; t < StateMenu::kTabs; ++t) {
            const ImGuiTabItemFlags tabFlags =
                stateMenu_.tabChanged && stateMenu_.tab == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(tr(kTabNames[t]), nullptr, tabFlags)) {
                // Clicked with the mouse.
                if (!stateMenu_.tabChanged && stateMenu_.tab != t) {
                    stateMenu_.tab = t;
                    stateMenu_.option = 0;
                    stateMenu_.changes.clear();
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    stateMenu_.tabChanged = false;
    theme::hint(tr("L1 / R1 (o Q / E): cambiar de pestaña   B / Esc: cerrar"));
    if (stateMenu_.tab == 0) drawStateSlots(state);
    else drawStateOptions(stateMenu_.tab, state, out);
    ImGui::End();
}

void Ui::drawStateSlots(const UiState& state) {
    if (!state.romLoaded) ImGui::TextDisabled("%s", tr("Abre una ROM para guardar estados."));
    theme::hint(tr("A / Intro: cargar   Y / Espacio: guardar"));
    ImGui::Separator();
    ImGui::BeginChild("ranuras");
    const float thumbH = 56.0f;
    for (int i = 0; i < static_cast<int>(state.slots.size()); ++i) {
        const SlotInfo& info = state.slots[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const bool selected = i == stateMenu_.selected;
        const ImVec2 start = ImGui::GetCursorPos();
        // The whole row is clickable with the mouse too: a click selects, a double click loads.
        if (ImGui::Selectable("##fila", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, thumbH))) {
            stateMenu_.selected = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && info.used && state.romLoaded) {
                stateMenu_.load = i;
                stateMenu_.open = false;
                settings_.stateSlot = i;
                pendingSave_ = true;
            }
        }
        if (selected && stateMenu_.scroll) {
            ImGui::SetScrollHereY(0.5f);
            stateMenu_.scroll = false;
        }
        ImGui::SetCursorPos(start);
        const float thumbW = info.height ? thumbH * static_cast<float>(info.width) / static_cast<float>(info.height) : thumbH * 1.43f;
        if (info.texture) ImGui::Image(static_cast<ImTextureID>(info.texture), ImVec2(thumbW, thumbH));
        else ImGui::Dummy(ImVec2(thumbW, thumbH));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text(i == settings_.stateSlot ? tr("Ranura %d  (actual)") : tr("Ranura %d"), i);
        if (info.used) ImGui::TextUnformatted(info.when.c_str());
        else ImGui::TextDisabled("%s", tr("vacía"));
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// The options that change while playing, as on the Vídeo window, the Sonido menu and the
// Emulación menu, in lines that the pad can drive. They change the general settings, or
// the game's own ones where it has them, as those windows do.
void Ui::drawStateOptions(int tab, const UiState& state, UiActions& out) {
    std::vector<MenuOption> options;
    RenderOptions& r = settings_.render;
    if (tab == 1) {
        // The view: the views main() has, by name.
        auto view = std::find_if(state.views.begin(), state.views.end(),
                                 [&](const char* v) { return settings_.view == v; });
        const int views = static_cast<int>(state.views.size());
        options.push_back({tr("Vista"), settings_.view, views > 1,
                           [this, &state, view, views](int dir) {
                               const int i = view == state.views.end() ? 0 : static_cast<int>(view - state.views.begin());
                               settings_.view = state.views[static_cast<size_t>((i + (dir < 0 ? views - 1 : 1)) % views)];
                           }});
        static const char* const kScales[] = {N_("Escala entera"), N_("Ajustar"), N_("Estirar")};
        options.push_back(choice(N_("Escala"), static_cast<int>(r.scale), kScales, 3,
                                 [&r](int v) { r.scale = static_cast<ScaleMode>(v); }));
        options.push_back(toggle(N_("Suavizar píxeles"), r.smooth));
        options.push_back(toggle(N_("Píxeles cuadrados (sin estirar a 4:3)"), r.squarePixels));
        static const char* const kFilters[] = {N_("Ninguno"), N_("Scanlines"), N_("CRT")};
        options.push_back(choice(N_("Filtro 2D"), static_cast<int>(r.filter), kFilters, 3,
                                 [&r](int v) { r.filter = static_cast<Filter2D>(v); }, kSave,
                                 r.upscale == Upscale2D::None));
        options.push_back(amount(N_("Intensidad del filtro"), r.filterStrength, 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f,
                                 kSave, r.filter != Filter2D::None && r.upscale == Upscale2D::None));
        static const char* const kUpscales[] = {N_("Ninguno"), "xBR", "Anime4K"};
        options.push_back(choice(N_("Reescalado (experimental)"), static_cast<int>(r.upscale), kUpscales, 3,
                                 [&r](int v) { r.upscale = static_cast<Upscale2D>(v); }));
        options.push_back(toggle(N_("Sin límite de sprites (sin parpadeo)"), settings_.unlimitedSprites));
        // The widescreen: the ready-made widths, then each side on its own.
        int preset = -1;
        for (int i = 0; i < kWidescreenCount; ++i)
            if (settings_.wideLeft == kWidescreens[i].columns && settings_.wideRight == kWidescreens[i].columns) preset = i;
        options.push_back({tr("Pantalla ancha"),
                           preset >= 0 ? std::string(kWidescreens[preset].name)
                                       : trf("%d / %d px", settings_.wideLeft, settings_.wideRight),
                           true,
                           [this, preset](int dir) {
                               const int next = preset < 0 ? (dir < 0 ? kWidescreenCount - 1 : 0)
                                                           : (preset + (dir < 0 ? kWidescreenCount - 1 : 1)) % kWidescreenCount;
                               settings_.wideLeft = settings_.wideRight = kWidescreens[next].columns;
                           },
                           kSave});
        options.push_back(number(N_("Izquierda##ancha"), settings_.wideLeft, 0, VideoFrame::kMaxWideSide, 4, "%.0f px"));
        options.push_back(number(N_("Derecha##ancha"), settings_.wideRight, 0, VideoFrame::kMaxWideSide, 4, "%.0f px"));
        options.push_back(toggle(N_("Pantalla completa (F11)"), settings_.fullscreen, kSave | kFullscreen));
        options.push_back(toggle(N_("Mostrar la barra de menús (Esc)"), settings_.showMenu));
        int scale = 0;
        for (int i = 0; i < kUiScaleCount; ++i)
            if (settings_.uiScale == kUiScales[i]) scale = i;
        options.push_back({tr("Tamaño de la interfaz"), uiScaleName(scale), true,
                           [this, scale](int dir) {
                               settings_.uiScale = kUiScales[(scale + (dir < 0 ? kUiScaleCount - 1 : 1)) % kUiScaleCount];
                           },
                           kSave});
    } else if (tab == 2) {
        options.push_back(toggle(N_("Efecto 3D"), r.pseudo3d));
        const bool on = r.pseudo3d;
        static const char* const kStyles[] = {N_("Vóxeles"), N_("Capas")};
        options.push_back(choice(N_("Estilo"), static_cast<int>(r.style), kStyles, 2,
                                 [&r](int v) { r.style = static_cast<Style3D>(v); }, kSave, on));
        options.push_back(amount(N_("Profundidad"), r.depth, 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f, kSave, on));
        options.push_back(toggle(N_("Inclinar la cámara"), r.tiltCamera, kSave, on));
        options.push_back(amount(N_("Inclinación"), r.tilt, kMinTilt, kMaxTilt, 5.0f, "%.0f°", 1.0f, kSave, on && r.tiltCamera));
        options.push_back(amount(N_("Giro"), r.yaw, -kMaxYaw, kMaxYaw, 5.0f, "%.0f°", 1.0f, kSave, on && r.tiltCamera));
        options.push_back(amount(N_("Zoom"), r.zoom, kMinZoom, kMaxZoom, 0.1f, "%.2fx", 1.0f, kSave, on));
        options.push_back(amount(N_("Grosor"), r.thickness, 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f, kSave, on));
        options.push_back(amount(N_("Extrusión"), r.extrusion, 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f, kSave,
                                 on && r.style == Style3D::Voxels));
        options.push_back(toggle(N_("Iluminación"), r.lighting, kSave, on));
        options.push_back(toggle(N_("Cámara viva"), r.liveCamera, kSave, on));
        options.push_back(toggle(N_("Sombra de los sprites"), r.shadows, kSave, on));
        options.push_back(toggle(N_("Plano B con prioridad delante"), r.highBInFront, kSave, on));
        options.push_back(toggle(N_("Plano B plano (como el cielo)"), r.flatB, kSave, on));
        options.push_back({tr("Valores por defecto"), tr("A: aplicar"), on, [&r](int dir) {
                               if (dir == 0) reset3D(r);
                           }, kSave, false});
    } else if (tab == 3) {
        options.push_back(number(N_("Volumen"), settings_.volume, 0, 100, 5, "%.0f%%"));
        options.push_back(toggle(N_("Silenciar"), settings_.mute));
    } else if (tab == 4) {
        const bool solo = !state.netSession;
        const bool canChange = !state.netPlaying && !state.hardcore;
        int overclock = 0;
        for (int k = 0; k < static_cast<int>(std::size(kOverclocks)); ++k)
            if (settings_.overclock >= kOverclocks[k]) overclock = k;
        options.push_back(choice(N_("Overclock"), overclock, kOverclockNames, static_cast<int>(std::size(kOverclocks)),
                                 [this](int v) { settings_.overclock = kOverclocks[v]; }, kSave, canChange));
        options.push_back({tr("Run-ahead"),
                           settings_.runAhead == 0   ? std::string(tr("desactivado"))
                           : settings_.runAhead == 1 ? std::string(tr("1 fotograma"))
                                                     : trf("%d fotogramas", settings_.runAhead),
                           true,
                           [this](int dir) {
                               settings_.runAhead = std::clamp(settings_.runAhead + (dir ? dir : 1), 0, Settings::kMaxRunAhead);
                           },
                           kSave});
        options.push_back({tr("Cámara lenta"), onOff(state.slowMotion), solo && !state.hardcore && state.romLoaded,
                           [&out](int) { out.toggleSlowMotion = true; }, 0});
        options.push_back(number(N_("Cámara lenta##velocidad"), settings_.slowMotion, 10, 90, 10, "%.0f%%"));
        options.push_back(number(N_("Avance rápido"), settings_.fastForward, 2, 10, 1, "%.0fx"));
        options.push_back({tr("Segundos de rebobinado"),
                           settings_.rewindSeconds ? trf("%d s", settings_.rewindSeconds) : std::string(tr("desactivado")),
                           true,
                           [this](int dir) {
                               settings_.rewindSeconds =
                                   std::clamp(settings_.rewindSeconds + (dir ? dir : 1) * 5, 0, Settings::kMaxRewindSeconds);
                           },
                           kSave | kRewind});
        options.push_back(toggle(N_("Contador de FPS (F)"), settings_.fpsCounter));
        options.push_back(choice(N_("Posición del contador"), settings_.fpsCorner, kFpsCornerNames, Settings::kFpsCorners,
                                 [this](int v) { settings_.fpsCorner = v; }, kSave, settings_.fpsCounter));
        if (state.hardcore) ImGui::TextDisabled("%s", tr("Modo hardcore: sin trucos, cámara lenta ni rebobinado"));
    } else {
        // What the Juego menu does, with A only: each closes the menu, and the press that
        // closed it does not reach the game (main() ignores the buttons still held).
        const bool solo = state.romLoaded && !state.netPlaying;  // as on the Juego menu
        options.push_back({tr("Pausa"), state.paused ? tr("A: continuar") : tr("A: pausar"), solo,
                           [this, &out](int dir) {
                               if (dir != 0) return;
                               out.togglePause = true;
                               stateMenu_.open = false;
                           }, 0, false});
        options.push_back({tr("Reiniciar"), tr("A: reiniciar"), solo, [this, &out](int dir) {
                               if (dir != 0) return;
                               out.reset = true;
                               stateMenu_.open = false;
                           }, 0, false});
        options.push_back({tr("Parar emulación"), tr("A: salir del juego"), state.romLoaded, [this, &out](int dir) {
                               if (dir != 0) return;
                               out.stop = true;
                               stateMenu_.open = false;
                           }, 0, false});
        // And what the menu bar's windows are for, so that a pad alone (a Steam Deck, a ROG
        // Ally...) reaches everything: each opens its window with the pad on it.
        auto open = [this](const char* label, const char* value, bool enabled, std::function<void()> act) {
            return MenuOption{tr(label), tr(value), enabled, [this, act = std::move(act)](int dir) {
                                  if (dir != 0) return;
                                  act();
                                  stateMenu_.open = false;
                              }, 0, false};
        };
        options.push_back(open(N_("Biblioteca de juegos"), N_("A: abrir"), true, [this] {
            if (!showLibrary_) toggleLibrary();
        }));
        options.push_back(open(N_("Abrir ROM"), N_("A: buscar el archivo"), true, [this] { openRequested_ = true; }));
        options.push_back(open(N_("Controles"), N_("A: abrir"), true, [this] { showControls_ = true; }));
        options.push_back(open(N_("Trucos"), N_("A: abrir"), solo && !state.hardcore, [this, &state, &out] {
            out.cheatsFor = state.romPath;
            showCheats_ = true;
        }));
        options.push_back(open(N_("Opciones de este juego"), N_("A: abrir"), solo, [this, &state, &out] {
            out.optionsFor = state.romPath;
            showGameOptions_ = true;
        }));
        options.push_back(open(N_("Menús (todas las opciones)"), N_("A: abrir la barra de menús"), true, [this] {
            // Shown for the pad, it hides again when the pad leaves it.
            menuForPad_ = !settings_.showMenu;
            settings_.showMenu = true;
            focusMenuBar_ = true;
        }));
        options.push_back({tr("Modo consola"), consoleModeName(settings_.consoleMode), true,
                           [this](int dir) { settings_.consoleMode = (settings_.consoleMode + (dir < 0 ? 2 : 1)) % 3; },
                           kSave});
        options.push_back(open(N_("Salir de plumnin"), N_("A: salir"), true, [&out] { out.quit = true; }));
        if (state.romLoaded && state.netPlaying)
            ImGui::TextDisabled("%s", tr("En una partida en red no se puede pausar ni reiniciar."));
    }

    const int count = static_cast<int>(options.size());
    stateMenu_.options = count;
    stateMenu_.option = std::clamp(stateMenu_.option, 0, std::max(0, count - 1));
    theme::hint(tr("Arriba / abajo: elegir   Izquierda / derecha: cambiar   A / Intro: activar"));
    ImGui::Separator();

    // The pad's and the keyboard's changes, and the mouse's below, all to one option.
    auto apply = [&](int index, int dir) {
        MenuOption& o = options[static_cast<size_t>(index)];
        if (!o.enabled || !o.change) return;
        o.change(dir);
        if (o.effects & kSave) out.settingsChanged = true;
        if (o.effects & kRewind) out.rewindChanged = true;
        if (o.effects & kFullscreen) out.fullscreenChanged = true;
    };
    // One change a frame, so that each option shows its new value before the next.
    if (!stateMenu_.changes.empty() && count > 0) {
        apply(stateMenu_.option, stateMenu_.changes.front());
        stateMenu_.changes.erase(stateMenu_.changes.begin());
    }

    ImGui::BeginChild("opciones");
    const float arrow = ImGui::GetFrameHeight();
    const float valueX = ImGui::GetContentRegionAvail().x * 0.56f;
    for (int i = 0; i < count; ++i) {
        const MenuOption& o = options[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const bool selected = i == stateMenu_.option;
        const ImVec2 start = ImGui::GetCursorPos();
        // A click chooses the option and a double click switches it; the arrows change it.
        if (ImGui::Selectable("##opcion", selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, arrow))) {
            stateMenu_.option = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) apply(i, 0);
        }
        if (selected && stateMenu_.scroll) {
            ImGui::SetScrollHereY(0.5f);
            stateMenu_.scroll = false;
        }
        ImGui::BeginDisabled(!o.enabled);
        ImGui::SetCursorPos(ImVec2(start.x + 4, start.y + ImGui::GetStyle().FramePadding.y));
        // Without what makes the label unique ("Cámara lenta##velocidad").
        ImGui::TextUnformatted(o.label.c_str(), o.label.c_str() + std::min(o.label.size(), o.label.find("##")));
        ImGui::SetCursorPos(ImVec2(start.x + valueX, start.y));
        if (!o.arrows) {
            ImGui::Dummy(ImVec2(arrow, arrow));
        } else if (ImGui::ArrowButton("##menos", ImGuiDir_Left)) {
            stateMenu_.option = i;
            apply(i, -1);
        }
        ImGui::SameLine();
        const float textW = ImGui::GetContentRegionAvail().x - arrow - ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 at = ImGui::GetCursorPos();
        if (!o.arrows) {
            // Nothing to change, only to do: a button with the mouse.
            if (ImGui::Button(o.value.c_str(), ImVec2(-FLT_MIN, 0))) {
                stateMenu_.option = i;
                apply(i, 0);
            }
        } else {
            const float w = ImGui::CalcTextSize(o.value.c_str()).x;
            ImGui::SetCursorPos(ImVec2(at.x + std::max(0.0f, (textW - w) * 0.5f), at.y + ImGui::GetStyle().FramePadding.y));
            ImGui::TextUnformatted(o.value.c_str());
            ImGui::SetCursorPos(ImVec2(at.x + textW, at.y));
            if (ImGui::ArrowButton("##mas", ImGuiDir_Right)) {
                stateMenu_.option = i;
                apply(i, 1);
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Ui::drawAbout() {
    if (!ImGui::Begin(tr("Acerca de plumnin"), &showAbout_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (iconTexture_) {
        const float w = ImGui::GetFontSize() * 6.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(iconTexture_), ImVec2(w, w));
    }
    if (logoTexture_) {
        const float w = ImGui::GetFontSize() * 22.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(logoTexture_), ImVec2(w, w * logoHeight_ / logoWidth_));
    }
    ImGui::TextUnformatted(tr("plumnin, emulador de Game Boy, Game Boy Color, NES y Super Nintendo con efecto pseudo-3D."));
    ImGui::TextDisabled("%s", trf("Versión %s", PLUMNIN_VERSION).c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Arrastra una ROM a la ventana o usa Archivo > Abrir ROM."));
    ImGui::SeparatorText(tr("Atajos"));
    ImGui::BulletText("%s", tr("Ctrl+O  abrir ROM;  F10  biblioteca de juegos"));
    ImGui::BulletText("%s", tr("F1  reiniciar;  F12  parar la emulación y volver al inicio"));
    ImGui::BulletText("%s", tr("F2  cambiar de vista"));
    ImGui::BulletText("%s", tr("Ctrl+R  grabar vídeo;  Ctrl+G  grabar GIF (otra vez para parar)"));
    ImGui::BulletText("%s", tr("P  pausa"));
    ImGui::BulletText("%s", tr("M  silenciar el sonido"));
    ImGui::BulletText("%s", tr("F11  pantalla completa"));
    ImGui::BulletText("%s", tr("Esc  mostrar u ocultar la barra de menús"));
    ImGui::BulletText("%s", tr("F3  efecto 3D (se puede cambiar en Opciones > Controles)"));
    ImGui::BulletText("%s", tr("F5 / F8  guardar / cargar estado; F6 / F7  cambiar de ranura"));
    ImGui::BulletText("%s", tr("F4 o clic del stick izquierdo  menú de estados, manejable con el mando"));
    ImGui::BulletText("%s", tr("Retroceso o gatillo izquierdo (mantener)  rebobinar"));
    ImGui::BulletText("%s", tr("Tab o gatillo derecho (mantener)  avance rápido;  F9  cámara lenta"));
    ImGui::BulletText("%s", tr("F  contador de FPS con su gráfica"));
    ImGui::SeparatorText(tr("Mando (teclado)"));
    ImGui::BulletText("%s", tr("Flechas  cruceta;  A S D  botones A B C;  Q W E  botones X Y Z"));
    ImGui::BulletText("%s", tr("Intro  Start;  Mayús derecha  Mode"));
    ImGui::End();
}

void Ui::drawError() {
    const char* const kTitle = "plumnin##error";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(error_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // The mouse may be hidden in full screen: Enter, Esc or the pad's A close it too.
    bool close = ImGui::Button(tr("Aceptar"), ImVec2(ImGui::GetFontSize() * 8.0f, 0));
    for (ImGuiKey key : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape, ImGuiKey_GamepadFaceDown})
        close |= ImGui::IsKeyPressed(key, false);
    if (close) {
        error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Ui::drawOverlays(const UiState& state) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    const float top = vp->Pos.y + static_cast<float>(menuHeight_);

    if (!state.romLoaded) {
        // Behind every window, so it never shows through the option windows.
        // The marquee above the hint, as wide as it is or 60% of the window if narrower.
        const char* hint = SDL_NumJoysticks() > 0
                               ? tr("Arrastra una ROM aquí, usa Archivo > Abrir ROM o abre la biblioteca (F10 o A en el mando)")
                               : tr("Arrastra una ROM aquí, usa Archivo > Abrir ROM o abre la biblioteca (F10)");
        const ImVec2 size = ImGui::CalcTextSize(hint);
        const float logoW = logoTexture_ ? std::min(static_cast<float>(logoWidth_), vp->Size.x * 0.6f) : 0.0f;
        const float logoH = logoTexture_ ? logoW * logoHeight_ / logoWidth_ : 0.0f;
        const float gap = logoTexture_ ? size.y * 1.5f : 0.0f;
        // The plum above it, if the window is tall enough to leave the game area some room.
        const float room = vp->Size.y - menuHeight_ - logoH - gap - size.y;
        const float iconS = iconTexture_ ? std::min(logoW * 0.45f, room * 0.45f) : 0.0f;
        const float iconGap = iconS > size.y * 3.0f ? size.y : 0.0f;
        const float plum = iconGap > 0.0f ? iconS : 0.0f;
        const float y = top + (room - plum - iconGap) * 0.5f;
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (plum > 0.0f) {
            const ImVec2 a(vp->Pos.x + (vp->Size.x - plum) * 0.5f, y);
            draw->AddImage(static_cast<ImTextureID>(iconTexture_), a, ImVec2(a.x + plum, a.y + plum));
        }
        const float logoY = y + plum + iconGap;
        if (logoTexture_) {
            const ImVec2 a(vp->Pos.x + (vp->Size.x - logoW) * 0.5f, logoY);
            draw->AddImage(static_cast<ImTextureID>(logoTexture_), a, ImVec2(a.x + logoW, a.y + logoH));
        }
        const ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, logoY + logoH + gap);
        draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), hint);
    }
    const char* badge = nullptr;
    if (state.romLoaded) {
        if (state.paused) badge = tr("PAUSA");
        else if (!state.speed.empty()) badge = state.speed.c_str();
    }
    if (state.romLoaded && state.recording) {
        // At the top in the middle: the achievements' corner is on the left, the speed's on the right.
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, top + 10), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##grabando", nullptr, flags);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", state.recording == 1 ? "GIF" : "REC");
        ImGui::SameLine();
        ImGui::Text("%d:%02d", state.recordingSeconds / 60, state.recordingSeconds % 60);
        ImGui::End();
    }
    if (badge) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 10, top + 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##pausa", nullptr, flags);
        ImGui::TextUnformatted(badge);
        ImGui::End();
    }
    if (state.romLoaded && settings_.fpsCounter && state.fps) drawFpsCounter(*state.fps, badge != nullptr);
    if (achievements_) achievements_->drawOverlay(top, showLibrary_ || stateMenu_.open || browser_.isOpen());

    if (!toast_.empty() && SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) toast_.clear();
    if (!toast_.empty()) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 10, vp->Pos.y + vp->Size.y - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##aviso", nullptr, flags);
        ImGui::TextUnformatted(toast_.c_str());
        ImGui::End();
    }
}

void Ui::drawFpsCounter(const FpsCounter& fps, bool badge) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const int corner = std::clamp(settings_.fpsCorner, 0, Settings::kFpsCorners - 1);
    const bool right = corner & 1, bottom = corner & 2;
    float y = bottom ? vp->Pos.y + vp->Size.y - 10 : vp->Pos.y + static_cast<float>(menuHeight_) + 10;
    if (!bottom && right && badge) y += ImGui::GetFrameHeightWithSpacing() + 8;  // below the speed badge
    if (bottom && !right && !toast_.empty()) y -= ImGui::GetFrameHeightWithSpacing() + 8;  // above the notices
    ImGui::SetNextWindowPos(ImVec2(right ? vp->Pos.x + vp->Size.x - 10 : vp->Pos.x + 10, y), ImGuiCond_Always,
                            ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    const double target = std::max(1.0, fps.target());
    const double budgetMs = 1000.0 / target;
    // Green at full speed, yellow a little below, red well below.
    auto speedColor = [&](double v) {
        return v >= target * 0.95 ? ImVec4(0.4f, 0.9f, 0.4f, 1) : v >= target * 0.75 ? ImVec4(1.0f, 0.85f, 0.3f, 1)
                                                                                     : ImVec4(1.0f, 0.4f, 0.35f, 1);
    };
    const ImVec4 emuColor(1.0f, 0.6f, 0.2f, 1), workColor(0.45f, 0.7f, 1.0f, 1);
    ImGui::TextColored(speedColor(fps.fps()), "%.1f FPS", fps.fps());
    ImGui::SameLine();
    ImGui::TextDisabled("/ %.0f   %s %.0f", target, tr("pantalla"), fps.screenFps());
    ImGui::TextColored(emuColor, "%s %.2f ms", tr("Emulación"), fps.emuMs());
    ImGui::SameLine();
    ImGui::TextDisabled("(%.0f %%)", fps.emuMs() / budgetMs * 100.0);
    ImGui::SameLine();
    ImGui::TextColored(workColor, "%s %.2f ms", tr("Total"), fps.workMs());

    // A graph of the last seconds: time on X (now on the right), the value on Y, with a
    // dashed line at the target (the console's rate, or the time one frame may take).
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float labelW = ImGui::CalcTextSize("0000").x + 4;
    auto graph = [&](const char* id, double yMax, double line, auto value, std::initializer_list<ImVec4> colors,
                     const char* unit) {
        const ImVec2 size(ImGui::GetFontSize() * 15.0f, ImGui::GetFontSize() * 3.6f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size.x + labelW, size.y));
        ImGui::PushID(id);
        const ImVec2 a(at.x + labelW, at.y), b(a.x + size.x, a.y + size.y);
        draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 110));
        const ImU32 axis = IM_COL32(200, 200, 200, 140);
        draw->AddLine(a, ImVec2(a.x, b.y), axis);
        draw->AddLine(ImVec2(a.x, b.y), b, axis);
        auto yOf = [&](double v) { return b.y - static_cast<float>(std::clamp(v / yMax, 0.0, 1.0)) * size.y; };
        char text[16];
        std::snprintf(text, sizeof text, "%.0f", yMax);
        draw->AddText(ImVec2(at.x, a.y - 2), axis, text);
        draw->AddText(ImVec2(at.x, b.y - ImGui::GetFontSize() + 2), axis, "0");
        draw->AddText(ImVec2(at.x, (a.y + b.y - ImGui::GetFontSize()) * 0.5f), axis, unit);
        const float ly = yOf(line);
        for (float x = a.x; x < b.x; x += 8) draw->AddLine(ImVec2(x, ly), ImVec2(std::min(x + 4, b.x), ly), IM_COL32(255, 255, 255, 120));
        const int n = fps.count();
        const float dx = size.x / static_cast<float>(FpsCounter::kPoints - 1);
        int series = 0;
        for (const ImVec4& c : colors) {
            ImVec2 prev;
            for (int i = 0; i < n; ++i) {
                const ImVec2 p(b.x - static_cast<float>(n - 1 - i) * dx, yOf(value(fps.point(i), series)));
                if (i) draw->AddLine(prev, p, ImGui::ColorConvertFloat4ToU32(c), 1.5f);
                prev = p;
            }
            ++series;
        }
        ImGui::PopID();
    };
    // The scales grow with what was measured, but not so much that a single hiccup (a
    // game being loaded) flattens the rest: what goes past the top is cut.
    double fpsMax = target * 1.25, msMax = budgetMs * 1.5;
    for (int i = 0; i < fps.count(); ++i) {
        fpsMax = std::max(fpsMax, static_cast<double>(fps.point(i).fps) * 1.1);
        msMax = std::max(msMax, static_cast<double>(std::max(fps.point(i).emuMs, fps.point(i).workMs)) * 1.1);
    }
    fpsMax = std::min(fpsMax, target * 4);
    msMax = std::min(msMax, budgetMs * 4);
    graph("fps", std::ceil(fpsMax / 10) * 10, target, [](const FpsCounter::Point& p, int) { return p.fps; },
          {speedColor(fps.fps())}, "FPS");
    graph("ms", std::ceil(msMax / 5) * 5, budgetMs,
          [](const FpsCounter::Point& p, int s) { return s == 0 ? p.workMs : p.emuMs; }, {workColor, emuColor}, "ms");
    ImGui::TextDisabled("%s", tr("-10 s  ·  ahora a la derecha"));
    ImGui::End();
}

void Ui::toggleLibrary() {
    showLibrary_ = !showLibrary_;
    if (!showLibrary_) return;
    if (!library_.scanned() || library_.folders() != settings_.libraryFolders) rescanLibrary();
    libraryView_.scroll = true;
}

void Ui::rescanLibrary() {
    library_.scan(settings_.libraryFolders);
    libraryView_.selected = 0;
    coversFirstFor_ = -1;
    libraryView_.scroll = true;
}

std::vector<size_t> Ui::visibleGames() {
    const std::string filter = searchKey(libraryView_.search);
    std::vector<size_t> out;
    const auto& games = library_.games();
    for (size_t i = 0; i < games.size(); ++i) {
        if (settings_.librarySystem >= 0 && static_cast<int>(games[i].system) != settings_.librarySystem) continue;
        if (filter.empty() || games[i].key.find(filter) != std::string::npos) out.push_back(i);
    }
    // The library is in title order; the other orders keep it among equals, and games
    // with nothing to sort by go last.
    const int sort = settings_.librarySort;
    const bool descending = libraryView_.descending;
    if (sort == 0) {
        if (descending) std::reverse(out.begin(), out.end());
        return out;
    }
    std::vector<std::pair<std::string, size_t>> keys;
    keys.reserve(out.size());
    for (size_t i : out) {
        const Library::Game& g = games[i];
        std::string key;
        if (sort == 1) {
            key = systemName(g.system);
        } else if (const GameDetails* d = gameInfo_.details(g.system, g.stem)) {
            if (sort == 2) key = d->year;
            else if (sort == 3) key = d->genre.empty() ? std::string() : searchKey(genreName(d->genre) ? tr(genreName(d->genre)) : d->genre);
            else key = searchKey(d->developer);
        }
        keys.emplace_back(std::move(key), i);
    }
    std::stable_sort(keys.begin(), keys.end(), [descending](const auto& a, const auto& b) {
        if (a.first.empty() != b.first.empty()) return b.first.empty();
        if (a.first != b.first) return descending ? a.first > b.first : a.first < b.first;
        return a.second < b.second;
    });
    for (size_t n = 0; n < keys.size(); ++n) out[n] = keys[n].second;
    return out;
}

void Ui::setLibraryView(int view) {
    if ((settings_.libraryList ? 1 : 0) == view) return;
    settings_.libraryList = view == 1;
    libraryView_.syncSort = true;
    libraryView_.scroll = true;
    pendingSave_ = true;
}

// While the library is open it owns the pad and the keyboard (except while typing in
// the search box): the arrows move through the grid, A (or Enter) plays the game, B
// goes back to the game if one is running (Esc closes it always), Y (or Ctrl+F) goes to the search
// box, Back changes the view, and the shoulder buttons (or Page Up and Page Down) change
// the console tab.
bool Ui::handleLibrary(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_KEYUP &&
        e.type != SDL_CONTROLLERBUTTONUP && e.type != SDL_TEXTINPUT)
        return false;
    if (browser_.isOpen() || showCheats_ || showGameOptions_) return false;
    const bool typing = ImGui::GetIO().WantTextInput;
    if (typing && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_CONTROLLERBUTTONUP) {
        // The search box gets the keys; Enter or the arrows hand them back to the grid.
        if (e.type == SDL_KEYDOWN && (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                                      e.key.keysym.scancode == SDL_SCANCODE_DOWN)) {
            ImGui::SetWindowFocus(nullptr);
            return true;
        }
        return false;
    }
    // Releases always reach ImGui, so it never thinks a key is still held.
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN) return e.type == SDL_TEXTINPUT;
    if (input_.hotkeyFor(e) == Hotkey::Library) {
        showLibrary_ = false;
        return true;
    }
    enum { None, Up, Down, Left, Right, Play, Close, Search, PrevTab, NextTab, Menu, View } action = None;
    if (e.type == SDL_KEYDOWN) {
        const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
        const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_APPLICATION: action = Menu; break;
            case SDL_SCANCODE_F10: if (shift) action = Menu; else return false; break;
            case SDL_SCANCODE_UP: action = Up; break;
            case SDL_SCANCODE_DOWN: action = Down; break;
            case SDL_SCANCODE_LEFT: action = Left; break;
            case SDL_SCANCODE_RIGHT: action = Right; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                if (e.key.keysym.mod & KMOD_ALT) return false;  // Alt+Intro is full screen
                action = Play;
                break;
            case SDL_SCANCODE_ESCAPE: action = Close; break;
            case SDL_SCANCODE_PAGEUP: action = PrevTab; break;
            case SDL_SCANCODE_PAGEDOWN: action = NextTab; break;
            case SDL_SCANCODE_F: if (ctrl) action = Search; break;
            case SDL_SCANCODE_V: if (!ctrl) action = View; else return false; break;
            default: return false;  // let ImGui have the rest (clicks on buttons, Tab...)
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: action = Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = Down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: action = Left; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: action = Right; break;
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: action = Play; break;
            case SDL_CONTROLLER_BUTTON_B: action = Close; break;
            case SDL_CONTROLLER_BUTTON_Y: action = Search; break;
            case SDL_CONTROLLER_BUTTON_X: action = Menu; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: action = PrevTab; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: action = NextTab; break;
            case SDL_CONTROLLER_BUTTON_BACK: action = View; break;
            default: break;
        }
    }
    // With a game's menu open the arrows move through it, A picks and B closes it.
    if (libraryView_.menuGame >= 0) {
        switch (action) {
            case Up: libraryView_.menuItem = (libraryView_.menuItem + LibraryView::kItems - 1) % LibraryView::kItems; break;
            case Down: libraryView_.menuItem = (libraryView_.menuItem + 1) % LibraryView::kItems; break;
            case Play: libraryView_.menuAction = libraryView_.menuItem; break;
            case Close: case Menu: libraryView_.menuGame = -1; break;
            default: break;
        }
        return true;
    }
    if (action == PrevTab || action == NextTab) {
        // Tabs: -1 (all), then each console; wraps around.
        int tab = settings_.librarySystem + 1 + (action == NextTab ? 1 : -1);
        tab = (tab + kSystemCount + 1) % (kSystemCount + 1);
        settings_.librarySystem = tab - 1;
        libraryView_.selected = 0;
        libraryView_.scroll = true;
        libraryView_.tabChanged = true;
        pendingSave_ = true;
        return true;
    }
    if (action == View) {
        setLibraryView(settings_.libraryList ? 0 : 1);
        return true;
    }
    const int count = static_cast<int>(visibleGames().size());
    const int cols = std::max(1, libraryView_.columns);
    // In the list, left and right go a page up or down.
    const int side = settings_.libraryList ? 10 : 1;
    int& sel = libraryView_.selected;
    switch (action) {
        case Up: if (sel - cols >= 0) sel -= cols; break;
        case Down: sel = std::min(count - 1, sel + cols); break;
        case Left: sel = std::max(0, sel - side); break;
        case Right: sel = std::min(count - 1, sel + side); break;
        case Play: if (count > 0) libraryView_.load = sel; break;
        // B goes back to the game; with none running there is nowhere to go back to, and
        // closing it would leave a pad alone on an empty screen (Esc still closes it).
        case Close:
            if (romLoaded_ || e.type == SDL_KEYDOWN) showLibrary_ = false;
            break;
        case Search: libraryView_.focusSearch = true; break;
        case Menu:
            if (count > 0) {
                libraryView_.menuGame = sel;
                libraryView_.menuItem = 0;
                libraryView_.openMenu = true;
            }
            break;
        default: break;
    }
    sel = std::clamp(sel, 0, std::max(0, count - 1));
    libraryView_.scroll = action == Up || action == Down || action == Left || action == Right;
    return true;
}

// The library's folders: each with a button to take it out, and one to add another.
void Ui::drawLibraryFolders(UiActions& out) {
    int remove = -1;
    for (int i = 0; i < static_cast<int>(settings_.libraryFolders.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton(tr("Quitar"))) remove = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(settings_.libraryFolders[static_cast<size_t>(i)].c_str());
        ImGui::PopID();
    }
    if (remove >= 0) {
        settings_.libraryFolders.erase(settings_.libraryFolders.begin() + remove);
        out.settingsChanged = true;
        rescanLibrary();
    }
    if (ImGui::Button(tr("Añadir carpeta..."))) {
        browseFor_ = BrowseFor::LibraryFolder;
        browser_.open(settings_.libraryFolders.empty() ? settings_.lastFolder : settings_.libraryFolders.back(), Pick::Folder);
    }
}

void Ui::drawLibrary(UiActions& out) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = static_cast<float>(menuHeight_);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin(tr("Biblioteca"), &showLibrary_, flags)) {
        ImGui::End();
        return;
    }
    if (pendingSave_) {
        out.settingsChanged = true;
        pendingSave_ = false;
    }
    auto& games = library_.games();

    if (settings_.libraryFolders.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Elige la carpeta donde tienes tus juegos de Game Boy, Game Boy Color, NES o Super Nintendo."));
        ImGui::TextDisabled("%s", tr("Se buscan también en sus subcarpetas, y luego puedes añadir más carpetas."));
        if (ImGui::Button(tr("Elegir carpeta..."))) {
            browseFor_ = BrowseFor::LibraryFolder;
            browser_.open(settings_.lastFolder, Pick::Folder);
        }
        ImGui::End();
        return;
    }

    const std::string foldersLabel = settings_.libraryFolders.size() == 1
                                         ? trf("Carpeta: %s", settings_.libraryFolders[0].c_str())
                                         : trf("%d carpetas", static_cast<int>(settings_.libraryFolders.size()));
    if (ImGui::TreeNodeEx("carpetas", showFolders_ ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", foldersLabel.c_str())) {
        showFolders_ = true;
        drawLibraryFolders(out);
        ImGui::TreePop();
    } else {
        showFolders_ = false;
    }
    if (ImGui::Button(tr("Volver a buscar"))) rescanLibrary();
    ImGui::SameLine();
    // Everything ScreenScraper has of the games it was not asked about yet.
    if (scraper_.batchLeft() > 0) {
        if (ImGui::Button(tr("Parar ScreenScraper"))) scraper_.cancelAll();
    } else if (ImGui::Button(tr("Descargar de ScreenScraper"))) {
        scrapeLibrary();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Busca en ScreenScraper (internet) cada juego que aún no tenga sus medios: portada completa,\n"
                                   "etiqueta del cartucho, capturas, logo... Las carátulas que ya hay se quedan, salvo\n"
                                   "que ScreenScraper tenga la portada completa. Uno a uno, así que tarda un poco."));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Abrir la biblioteca al empezar"), &settings_.libraryAtStart)) out.settingsChanged = true;

    // One tab per console, with how many games each has.
    int counts[kSystemCount] = {};
    for (const auto& g : games) ++counts[static_cast<int>(g.system)];
    if (ImGui::BeginTabBar("sistemas")) {
        for (int tab = -1; tab < kSystemCount; ++tab) {
            const int n = tab < 0 ? static_cast<int>(games.size()) : counts[tab];
            const std::string label = std::string(tab < 0 ? tr("Todos") : systemName(static_cast<ConsoleSystem>(tab))) +
                                      " (" + std::to_string(n) + ")###tab" + std::to_string(tab);
            // Tabs changed with the pad or the keyboard are selected by hand.
            const ImGuiTabItemFlags f =
                libraryView_.tabChanged && settings_.librarySystem == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, f)) {
                if (!libraryView_.tabChanged && settings_.librarySystem != tab) {
                    settings_.librarySystem = tab;
                    libraryView_.selected = 0;
                    out.settingsChanged = true;
                }
                ImGui::EndTabItem();
            }
        }
        libraryView_.tabChanged = false;
        ImGui::EndTabBar();
    }

    if (libraryView_.focusSearch) {
        ImGui::SetKeyboardFocusHere();
        libraryView_.focusSearch = false;
    }
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##buscar", tr("Buscar (Ctrl+F o Y)"), libraryView_.search, sizeof libraryView_.search))
        libraryView_.selected = 0;
    // How the games are shown: a grid or a list, in what order, and with the panel or not.
    ImGui::SameLine();
    const int libraryView = settings_.libraryList ? 1 : 0;
    if (ImGui::RadioButton(tr("Cuadrícula"), libraryView == 0)) setLibraryView(0);
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Lista"), libraryView == 1)) setLibraryView(1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Cambia de vista con V o con Select en el mando."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    static const char* const sorts[Settings::kLibrarySorts] = {N_("Por título"), N_("Por consola"), N_("Por año"),
                                                               N_("Por género"), N_("Por desarrolladora")};
    if (ImGui::BeginCombo("##orden", tr(sorts[std::clamp(settings_.librarySort, 0, Settings::kLibrarySorts - 1)]))) {
        for (int i = 0; i < Settings::kLibrarySorts; ++i)
            if (ImGui::Selectable(tr(sorts[i]), settings_.librarySort == i)) {
                settings_.librarySort = i;
                libraryView_.descending = i == 2;  // the newest first
                libraryView_.syncSort = true;
                out.settingsChanged = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Detalles"), &settings_.libraryDetails)) out.settingsChanged = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Un panel a la derecha con la carátula, los datos y la descripción del juego elegido."));
    const std::vector<size_t> visible = visibleGames();
    // A new order (or the data to sort by arriving) keeps the same game chosen.
    const unsigned order = static_cast<unsigned>(settings_.librarySort) * 2 + libraryView_.descending + 2 +
                           (settings_.librarySort ? gameInfo_.detailsVersion() * 16 : 0);
    if (order != libraryView_.order) {
        libraryView_.order = order;
        const auto it = std::find(visible.begin(), visible.end(), libraryView_.selectedGame);
        if (it != visible.end()) {
            libraryView_.selected = static_cast<int>(it - visible.begin());
            libraryView_.scroll = true;
        }
    }
    if (libraryView_.selected >= 0 && libraryView_.selected < static_cast<int>(visible.size()))
        libraryView_.selectedGame = visible[static_cast<size_t>(libraryView_.selected)];
    if (library_.pending() > 0) ImGui::Text(tr("%d juegos. Buscando carátulas, quedan %d..."), static_cast<int>(visible.size()), library_.pending());
    else ImGui::Text(tr("%d juegos."), static_cast<int>(visible.size()));
    // Games still without a cover: one click away from ScreenScraper.
    if (library_.pending() == 0 && scraper_.batchLeft() == 0 && !scraper_.batchFailed()) {
        int noCover = 0;
        for (const auto& g : games) noCover += g.cover == Library::Cover::Missing;
        if (noCover > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", trf(noCover == 1 ? N_("· %d sin carátula") : N_("· %d sin carátulas"), noCover).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Descargarlas de ScreenScraper"))) scrapeLibrary();
        }
    }
    // ScreenScraper's round of downloads, on a line of its own: how it goes, or why it
    // stopped (the account, the connection...), in colour.
    if (const std::string ss = scraper_.batchStatus(); !ss.empty()) {
        if (scraper_.batchFailed()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", ss.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(tr("Cuenta de ScreenScraper..."))) openScraping(0);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Volver a intentarlo"))) scrapeLibrary();
        } else {
            ImGui::TextDisabled("%s", ss.c_str());
        }
    }

    // The covers' size, at the right end of that line: smaller ones fit more to a row.
    if (libraryView == 0) {
        const char* label = tr("Tamaño");
        const float sliderW = 160.0f;
        const float w = sliderW + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
        ImGui::SetNextItemWidth(sliderW);
        ImGui::SliderInt(label, &settings_.libraryCoverWidth, Settings::kMinCoverWidth, Settings::kMaxCoverWidth, "",
                         ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("El tamaño de las carátulas: más pequeñas caben más en cada fila."));
    }
    // B only closes it with a game to go back to (handleLibrary); Esc always does.
    theme::hint(romLoaded_
                    ? tr("Flechas o cruceta: elegir   A o Intro: jugar   X, botón derecho o tecla de menú: trucos y opciones\n"
                         "B o Esc: volver al juego   L / R o RePág / AvPág: consola   Select o V: cuadrícula o lista")
                    : tr("Flechas o cruceta: elegir   A o Intro: jugar   X, botón derecho o tecla de menú: trucos y opciones\n"
                         "Esc: cerrar   L / R o RePág / AvPág: consola   Select o V: cuadrícula o lista"));
    ImGui::Separator();

    if (games.empty()) ImGui::TextDisabled("%s", tr("No hay juegos (.gb, .gbc, .nes, .sfc, .smc, .zip) en estas carpetas."));
    else if (visible.empty() && libraryView_.search[0]) ImGui::TextDisabled("%s", tr("Ningún juego coincide con la búsqueda."));
    else if (visible.empty()) ImGui::TextDisabled("%s", tr("No hay juegos de esta consola en tus carpetas."));

    // The games on the left (a grid of covers or a list), the chosen one's data on the right.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float full = ImGui::GetContentRegionAvail().x;
    const float panelW = settings_.libraryDetails ? std::clamp(full * 0.3f, 280.0f, 460.0f) : 0.0f;
    ImGui::BeginChild("juegos", ImVec2(panelW > 0 ? full - panelW - style.ItemSpacing.x : 0.0f, 0.0f));
    // While covers are still being looked for, the ones around the chosen game (what is on
    // screen) go first, the nearest first.
    if (library_.pending() > 0 && !visible.empty() &&
        (libraryView_.selected != coversFirstFor_ || visible.size() != coversFirstCount_)) {
        coversFirstFor_ = libraryView_.selected;
        coversFirstCount_ = visible.size();
        const int n = static_cast<int>(visible.size());
        const int at = std::clamp(libraryView_.selected, 0, n - 1);
        std::vector<size_t> first;
        for (int d = 0; d <= 48; ++d) {
            if (at + d < n) first.push_back(visible[static_cast<size_t>(at + d)]);
            if (d > 0 && at - d >= 0) first.push_back(visible[static_cast<size_t>(at - d)]);
        }
        library_.prioritize(first);
    }
    if (settings_.libraryList) drawLibraryList(visible);
    else drawLibraryGrid(visible);
    ImGui::EndChild();
    if (panelW > 0) {
        ImGui::SameLine();
        ImGui::BeginChild("detalles", ImVec2(panelW, 0.0f), ImGuiChildFlags_Borders);
        drawLibraryDetails(visible);
        ImGui::EndChild();
    }

    // The game's menu, opened with the right button, X on the pad or the menu key.
    if (libraryView_.openMenu) {
        ImGui::OpenPopup("menu_juego");
        libraryView_.openMenu = false;
    }
    if (ImGui::BeginPopup("menu_juego")) {
        const int n = libraryView_.menuGame;
        if (n < 0 || n >= static_cast<int>(visible.size())) {
            ImGui::CloseCurrentPopup();
        } else {
            const Library::Game& g = games[visible[static_cast<size_t>(n)]];
            ImGui::TextDisabled("%s", g.title.c_str());
            ImGui::Separator();
            static const char* items[LibraryView::kItems] = {N_("Jugar"), N_("Trucos..."), N_("Opciones de este juego..."),
                                                             N_("Buscar en ScreenScraper por nombre..."),
                                                             N_("Carátula desde imagen..."), N_("Medios de ScreenScraper...")};
            int picked = libraryView_.menuAction;
            for (int i = 0; i < LibraryView::kItems; ++i)
                if (ImGui::Selectable(tr(items[i]), libraryView_.menuItem == i, ImGuiSelectableFlags_NoAutoClosePopups)) picked = i;
            switch (picked) {
                case 0: libraryView_.load = n; break;
                case 1: out.cheatsFor = g.path; showCheats_ = true; break;
                case 2: out.optionsFor = g.path; showGameOptions_ = true; break;
                case 3: startMedia(visible[static_cast<size_t>(n)], 2); break;  // ScreenScraper's search by name
                case 4: chooseCoverImage(visible[static_cast<size_t>(n)]); break;
                case 5: openMedia(visible[static_cast<size_t>(n)]); break;
                default: break;
            }
            if (picked >= 0) {
                libraryView_.menuGame = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    } else {
        libraryView_.menuGame = -1;  // closed by clicking elsewhere
    }
    libraryView_.menuAction = -1;

    if (libraryView_.load >= 0 && libraryView_.load < static_cast<int>(visible.size())) {
        out.load = games[visible[static_cast<size_t>(libraryView_.load)]].path;
        showLibrary_ = false;
    }
    libraryView_.load = -1;
    ImGui::End();
}

// What clicking a game does, in the grid and in the list: a click picks it, a double
// click plays it, and the right button opens its menu.
void Ui::libraryItem(bool clicked, int n) {
    if (clicked) {
        libraryView_.selected = n;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) libraryView_.load = n;
    }
    // On release, like ImGui's own context menus: the press closes open popups.
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        libraryView_.selected = n;
        libraryView_.menuGame = n;
        libraryView_.menuItem = 0;
        libraryView_.openMenu = true;
    }
}

void Ui::drawLibraryGrid(const std::vector<size_t>& visible) {
    const auto& games = library_.games();
    const float coverW = static_cast<float>(settings_.libraryCoverWidth), coverH = std::round(coverW * 1.4f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float textH = ImGui::GetTextLineHeight() * 2 + 4;
    const ImVec2 card(coverW + 12, coverH + textH + 12);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + style.ItemSpacing.x) / (card.x + style.ItemSpacing.x)));
    libraryView_.columns = cols;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    theme::libraryBackground(inner.Min, inner.Max);
    for (int n = 0; n < static_cast<int>(visible.size()); ++n) {
        const Library::Game& g = games[visible[static_cast<size_t>(n)]];
        if (n % cols) ImGui::SameLine();
        ImGui::PushID(n);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool selected = n == libraryView_.selected;
        libraryItem(ImGui::Selectable("##juego", selected, ImGuiSelectableFlags_AllowDoubleClick, card), n);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", systemName(g.system), g.path.c_str());
        if (selected && libraryView_.scroll) {
            ImGui::SetScrollHereY(0.5f);
            libraryView_.scroll = false;
        }
        // The cover, fitted into its box; a plain box with the console's name if there is none.
        const ImVec2 box(pos.x + 6, pos.y + 6);
        if (g.texture && g.height > 0) {
            const float u0 = 0;
            const float gw = static_cast<float>(g.width);
            float w = coverW, h = coverW * static_cast<float>(g.height) / gw;
            if (h > coverH) {
                h = coverH;
                w = coverH * gw / static_cast<float>(g.height);
            }
            const ImVec2 a(box.x + (coverW - w) * 0.5f, box.y + (coverH - h));
            draw->AddImage(static_cast<ImTextureID>(g.texture), a, ImVec2(a.x + w, a.y + h), ImVec2(u0, 0), ImVec2(1, 1));
        } else {
            draw->AddRectFilled(box, ImVec2(box.x + coverW, box.y + coverH), IM_COL32(0x15, 0x15, 0x17, 255));
            draw->AddRect(box, ImVec2(box.x + coverW, box.y + coverH), IM_COL32(0x44, 0x44, 0x4B, 255));
            const char* label = g.cover == Library::Cover::Pending ? "..." : systemName(g.system);
            const ImVec2 size = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(box.x + (coverW - size.x) * 0.5f, box.y + (coverH - size.y) * 0.5f),
                          IM_COL32(140, 146, 160, 255), label);
        }
        // On the "Todos" tab, a badge on each cover says which console.
        if (settings_.librarySystem < 0) {
            const char* badge = systemName(g.system);
            const ImVec2 size = ImGui::CalcTextSize(badge);
            const ImVec2 b0(box.x + coverW - size.x - 10, box.y + 4);
            const ImU32 colour = g.system == ConsoleSystem::GameBoy        ? IM_COL32(90, 110, 40, 230)
                                 : g.system == ConsoleSystem::GameBoyColor ? IM_COL32(120, 70, 170, 230)
                                 : g.system == ConsoleSystem::Nes          ? IM_COL32(200, 60, 50, 230)
                                                                          : IM_COL32(40, 110, 200, 230);
            draw->AddRectFilled(b0, ImVec2(b0.x + size.x + 8, b0.y + size.y + 4), colour, 3.0f);
            draw->AddText(ImVec2(b0.x + 4, b0.y + 2), IM_COL32(255, 255, 255, 255), badge);
        }
        if (selected)
            draw->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + card.x - 1, pos.y + card.y - 1),
                          ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 3.0f);
        // Two lines of title, cut to the card's width, on a plate so they stand out
        // from the background; the tags in grey after it.
        const ImVec4 clip(pos.x + 4, pos.y, pos.x + card.x - 4, pos.y + card.y);
        if (!selected && !ImGui::IsItemHovered())
            draw->AddRectFilled(ImVec2(box.x - 2, box.y + coverH + 2), ImVec2(box.x + coverW + 2, pos.y + card.y - 4),
                                IM_COL32(0x05, 0x05, 0x06, 235));
        draw->AddText(nullptr, 0.0f, ImVec2(pos.x + 6, box.y + coverH + 4), ImGui::GetColorU32(ImGuiCol_Text),
                      g.title.c_str(), nullptr, coverW, &clip);
        ImGui::PopID();
    }
}

// The games' boxes on a shelf in 3D: a click (or A) takes one out, a second its cartridge
// and a third puts that into the console to play; dragging turns what is out, or moves
// along the shelf, and the wheel zooms.
void Ui::drawLibraryList(const std::vector<size_t>& visible) {
    const auto& games = library_.games();
    libraryView_.columns = 1;
    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("lista", 6, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Título"), ImGuiTableColumnFlags_DefaultSort, 3.0f, 0);
    ImGui::TableSetupColumn(tr("Consola"), 0, 1.0f, 1);
    ImGui::TableSetupColumn(tr("Año"), ImGuiTableColumnFlags_PreferSortDescending, 0.5f, 2);
    ImGui::TableSetupColumn(tr("Género"), 0, 1.1f, 3);
    ImGui::TableSetupColumn(tr("Desarrolladora"), 0, 1.4f, 4);
    ImGui::TableSetupColumn(tr("Jugadores"), ImGuiTableColumnFlags_NoSort, 0.6f, 5);
    // The order chosen elsewhere (the settings, "Ordenar por") shown on the headers;
    // the columns are in the same order as the sorts.
    if (libraryView_.syncSort) {
        ImGui::TableSetColumnSortDirection(settings_.librarySort,
                                           libraryView_.descending ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending,
                                           false);
        libraryView_.syncSort = false;
    }
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
            const int sort = static_cast<int>(specs->Specs[0].ColumnUserID);
            const bool descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            if (sort != settings_.librarySort || descending != libraryView_.descending) {
                settings_.librarySort = sort;
                libraryView_.descending = descending;
                pendingSave_ = true;
            }
        }
        specs->SpecsDirty = false;
    }
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    if (libraryView_.scroll && libraryView_.selected < static_cast<int>(visible.size()))
        clipper.IncludeItemByIndex(libraryView_.selected);
    while (clipper.Step()) {
        for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
            const Library::Game& g = games[visible[static_cast<size_t>(n)]];
            const GameDetails* d = gameInfo_.details(g.system, g.stem);
            const bool selected = n == libraryView_.selected;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(n);
            libraryItem(ImGui::Selectable(g.title.c_str(), selected,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick),
                        n);
            if (ImGui::IsItemHovered() && !g.tags.empty()) ImGui::SetTooltip("%s\n%s", g.tags.c_str(), g.path.c_str());
            if (selected && libraryView_.scroll) {
                ImGui::SetScrollHereY(0.5f);
                libraryView_.scroll = false;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(systemName(g.system));
            if (d) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(d->year.c_str());
                ImGui::TableNextColumn();
                const char* genre = genreName(d->genre);
                ImGui::TextUnformatted(genre ? tr(genre) : d->genre.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(d->developer.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(d->players.c_str());
            }
        }
    }
    ImGui::EndTable();
}

// The chosen game: its cover, a button to play it, libretro-database's data, and
// ScreenScraper's screenshot and description.
void Ui::drawLibraryDetails(const std::vector<size_t>& visible) {
    const int n = libraryView_.selected;
    if (n < 0 || n >= static_cast<int>(visible.size())) {
        gameInfo_.show({}, {}, {}, ConsoleSystem::Nes);
        ImGui::TextDisabled("%s", tr("Elige un juego para ver sus datos."));
        return;
    }
    const Library::Game& g = library_.games()[visible[static_cast<size_t>(n)]];
    gameInfo_.show(g.path, g.stem, g.title, g.system);
    const GameInfo::Sheet& sheet = gameInfo_.sheet();
    const float width = ImGui::GetContentRegionAvail().x;
    // The cover takes at most 40% of the panel's height, so the data shows below it.
    const float coverMax = std::clamp(ImGui::GetContentRegionAvail().y * 0.4f, 140.0f, 320.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    needGlyphs(g.title);
    ImGui::PushFont(theme::fonts().title);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextWrapped("%s", g.title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::TextDisabled("%s%s%s", systemName(g.system), g.tags.empty() ? "" : " · ", g.tags.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.path.c_str());

    // An image fitted into the panel's width and at most `maxH` tall, centred.
    // From `u0` across: a whole sleeve shows only its front.
    auto image = [&](unsigned int texture, int w, int h, float maxH, float u0 = 0) {
        const float fw = static_cast<float>(w) * (1 - u0);
        float iw = width, ih = width * static_cast<float>(h) / fw;
        if (ih > maxH) {
            ih = maxH;
            iw = maxH * fw / static_cast<float>(h);
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - iw) * 0.5f);
        ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(iw, ih), ImVec2(u0, 0), ImVec2(1, 1));
    };
    ImGui::Spacing();
    if (g.texture && g.width > 0 && g.height > 0) {
        image(g.texture, g.width, g.height, coverMax);
    } else {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float h = std::min(width * 1.4f, coverMax);
        draw->AddRectFilled(a, ImVec2(a.x + width, a.y + h), IM_COL32(0x26, 0x26, 0x2A, 255));
        draw->AddRect(a, ImVec2(a.x + width, a.y + h), IM_COL32(0x44, 0x44, 0x4B, 255));
        const char* label = g.cover == Library::Cover::Pending ? "..." : tr("Sin carátula");
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(a.x + (width - size.x) * 0.5f, a.y + (h - size.y) * 0.5f), IM_COL32(140, 146, 160, 255), label);
        ImGui::Dummy(ImVec2(width, h));
    }
    ImGui::Spacing();
    if (theme::primaryButton(tr("Jugar"))) libraryView_.load = n;

    // libretro-database's data, the lines it has.
    ImGui::Spacing();
    if (const GameDetails* d = gameInfo_.details(g.system, g.stem)) {
        const char* genre = genreName(d->genre);
        const std::pair<const char*, std::string> rows[] = {
            {N_("Desarrolladora"), d->developer},
            {N_("Distribuidora"), d->publisher},
            {N_("Género"), genre ? tr(genre) : d->genre},
            {N_("Año"), d->year},
            {N_("Jugadores"), d->players},
        };
        // The labels' column as wide as the widest from the first frame: fitted only after
        // one, the values would wrap a letter per line until then.
        float labels = 0;
        for (const auto& row : rows)
            if (!row.second.empty()) labels = std::max(labels, ImGui::CalcTextSize(tr(row.first)).x);
        if (ImGui::BeginTable("datos", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##campo", ImGuiTableColumnFlags_WidthFixed, labels);
            ImGui::TableSetupColumn("##valor", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [label, value] : rows) {
                if (value.empty()) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", tr(label));
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
            }
            ImGui::EndTable();
        }
    } else if (gameInfo_.detailsBusy()) {
        ImGui::TextDisabled("%s", tr("Descargando los datos de los juegos..."));
    }

    if (sheet.snap && sheet.width > 0 && sheet.height > 0) {
        ImGui::Spacing();
        image(sheet.snap, sheet.width, sheet.height, coverMax);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Descripción"));
    if (sheet.busy) {
        ImGui::TextDisabled("...");
        return;
    }
    if (!sheet.description.empty()) {
        needGlyphs(sheet.description);
        ImGui::TextWrapped("%s", sheet.description.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Fuente: ScreenScraper"));
    } else {
        ImGui::TextWrapped("%s", sheet.status.c_str());
        if (!sheet.known && ImGui::Button(tr("Descargar de ScreenScraper"))) startMedia(visible[static_cast<size_t>(n)], 1);
    }
    if (ImGui::Button(tr("Medios de ScreenScraper..."))) openMedia(visible[static_cast<size_t>(n)]);
}

void Ui::chooseCoverImage(size_t game) {
    const auto& games = library_.games();
    if (game >= games.size()) return;
    coverImageFor_ = games[game].path;
    const std::string start = coverImageFor_.substr(0, coverImageFor_.find_last_of("/\\"));
    std::string chosen;
    browseFor_ = BrowseFor::CoverImage;
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, start, Pick::Image, &chosen)) {
        case DialogResult::Chosen: useCoverImage(chosen); break;
        case DialogResult::Cancelled: break;
        case DialogResult::Unavailable: browser_.open(start, Pick::Image); break;
    }
}

// The game is found again by its path: the library may have been scanned meanwhile.
void Ui::useCoverImage(const std::string& file) {
    const auto& games = library_.games();
    for (size_t i = 0; i < games.size(); ++i)
        if (games[i].path == coverImageFor_) {
            std::string error;
            if (!library_.coverFromFile(i, file, &error) && !error.empty()) showError(error);
            break;
        }
    coverImageFor_.clear();
}

void Ui::drawNetplay(const UiState& state, UiActions& out) {
    if (!ImGui::Begin(tr("Jugar en red"), &showNetplay_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    if (ImGui::IsWindowAppearing()) {
        localAddresses_ = UdpTransport::localAddresses();
        std::snprintf(netAddress_, sizeof netAddress_, "%s", settings_.netAddress.c_str());
    }
    if (state.netSession) {
        ImGui::TextWrapped("%s", state.netStatus.c_str());
        if (state.netProgress >= 0) ImGui::ProgressBar(state.netProgress, ImVec2(ImGui::GetFontSize() * 36.0f, 0));
        ImGui::Spacing();
        if (ImGui::Button(tr("Salir de la partida"))) out.netLeave = true;
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", tr("Dos jugadores, cada uno en su ordenador y con el mismo juego abierto (la misma ROM). "
                       "Quien aloja es el jugador 1 y quien se une, el jugador 2; cada uno juega con sus "
                       "controles del jugador 1. La partida empieza donde la tenga el anfitrión."));
    if (!state.romLoaded)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", tr("Abre primero el juego."));
    ImGui::BeginDisabled(!state.romLoaded);

    ImGui::SeparatorText(tr("Alojar la partida (jugador 1)"));
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(tr("Puerto UDP"), &settings_.netPort, 0, 0)) settings_.netPort = std::clamp(settings_.netPort, 1, 65535);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Alojar"))) out.netHost = out.settingsChanged = true;
    if (!localAddresses_.empty())
        ImGui::TextWrapped(tr("En casa, tu amigo se une a %s:%d."), localAddresses_.front().c_str(), settings_.netPort);
    ImGui::TextDisabled(tr("Por internet: abre (redirige) el puerto UDP %d de tu router hacia este ordenador y "
                        "dale tu IP pública; o usad los dos una red privada como Tailscale o ZeroTier "
                        "y no hace falta abrir nada."), settings_.netPort);

    ImGui::SeparatorText(tr("Unirse a una partida (jugador 2)"));
    ImGui::SetNextItemWidth(300);
    const bool enter = ImGui::InputTextWithHint(tr("Anfitrión"), tr("IP o nombre, y :puerto si no es el de arriba"),
                                                netAddress_, sizeof netAddress_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(tr("Unirse")) || enter) && netAddress_[0]) {
        settings_.netAddress = netAddress_;
        out.netJoin = out.settingsChanged = true;
    }

    ImGui::SeparatorText(tr("Ajustes"));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("Retardo de los mandos"), &settings_.netInputDelay, 0, 6, tr("%d fotogramas"), ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Cuántos fotogramas tarda en hacer efecto lo que pulsas. Con más retardo hay menos\n"
                          "saltos cuando la conexión es lenta; con 0 responde al instante pero salta más.\n"
                          "Vale el del anfitrión. 2 va bien casi siempre."));
    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void Ui::drawCheats(const UiState& state, UiActions& out) {
    const CheatDecoder decoder = cheatDecoderFor(state.cheatsSystem);
    ImGui::SetNextWindowSize(ImVec2(std::min(theme::px(560), ImGui::GetMainViewport()->Size.x - 20), 0), ImGuiCond_Appearing);
    if (cheatsRefit_ > 0) ImGui::SetNextWindowSize(ImVec2(cheatsWidth_, 0));  // grow to fit the list that arrived
    if (!ImGui::Begin(tr("Trucos"), &showCheats_)) {
        ImGui::End();
        return;
    }
    if (!state.cheats) {
        ImGui::TextDisabled("%s", tr("Abre una ROM, o elige un juego en la biblioteca con el botón derecho."));
        ImGui::End();
        return;
    }
    std::vector<Cheat>& cheats = *state.cheats;
    if (const char* formats = cheatFormatsFor(state.cheatsSystem); formats[0])
        ImGui::TextWrapped(tr("Códigos %s de %s. Se guardan para este juego y se activan al abrirlo."), tr(formats),
                           state.cheatsGame.c_str());
    else
        ImGui::TextWrapped(tr("plumnin todavía no lee los trucos de la %s."), systemName(state.cheatsSystem));
    ImGui::Spacing();
    int remove = -1;
    if (cheats.empty()) ImGui::TextDisabled("%s", tr("Este juego no tiene trucos todavía."));
    else if (ImGui::BeginTable("trucos", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn(tr("Código"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn(tr("Nombre"));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        for (int i = 0; i < static_cast<int>(cheats.size()); ++i) {
            Cheat& c = cheats[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##activo", &c.enabled)) out.cheatsChanged = true;
            ImGui::TableNextColumn();
            std::vector<CheatPatch> patches;
            std::string error;
            const bool valid = decodeCheatCodes(c.code, decoder, &patches, &error);
            const std::string shown = shortCode(c.code);
            if (valid) ImGui::TextUnformatted(shown.c_str());
            else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", shown.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", valid ? describePatches(patches).c_str() : error.c_str());
            ImGui::TableNextColumn();
            char name[96];
            std::snprintf(name, sizeof name, "%s", c.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##nombre", name, sizeof name)) c.name = name;
            if (ImGui::IsItemDeactivatedAfterEdit()) out.cheatsChanged = true;
            ImGui::TableNextColumn();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        cheats.erase(cheats.begin() + remove);
        out.cheatsChanged = true;
    }

    ImGui::SeparatorText(tr("Añadir"));
    ImGui::SetNextItemWidth(130);
    bool add = ImGui::InputTextWithHint("##codigo", tr("Código"), cheatCode_, sizeof cheatCode_,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Añadir")).x - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    add |= ImGui::InputTextWithHint("##nombre_nuevo", tr("Nombre (opcional)"), cheatName_, sizeof cheatName_,
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    add |= ImGui::Button(tr("Añadir"));
    if (add) {
        std::vector<CheatPatch> patches;
        if (decodeCheatCodes(cheatCode_, decoder, &patches, &cheatError_)) {
            cheats.push_back({normalizeCheat(cheatCode_), cheatName_, true});
            cheatCode_[0] = cheatName_[0] = '\0';
            cheatError_.clear();
            out.cheatsChanged = true;
        }
    }
    if (!cheatError_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", cheatError_.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("Los Game Genie cambian la ROM; los Action Replay y GameShark fijan un valor en la RAM en cada "
                       "cuadro. Algunos juegos comprueban la ROM al arrancar: si no arranca con un truco, reinicia sin él."));
    ImGui::PopStyleColor();
    drawCheatDatabase(state, out);
    ImGui::End();
}

// The game's ready-made cheat lists from libretro-database, looked for when the window
// is opened for it: pick a list (the one named like the ROM comes first) and add codes.
void Ui::drawCheatDatabase(const UiState& state, UiActions& out) {
    const CheatDecoder decoder = cheatDecoderFor(state.cheatsSystem);
    ImGui::SeparatorText(tr("Trucos de internet"));
    if (cheatDb_.view().game != state.cheatsPath) {
        cheatFilter_[0] = '\0';
        cheatDb_.search(state.cheatsPath);
    }
    const bool wasBusy = cheatDb_.view().busy;
    cheatDb_.update();
    const CheatDatabase::View& db = cheatDb_.view();
    // Two frames: the first lays the list out, the second fits the window to it.
    if (cheatsRefit_ > 0) --cheatsRefit_;
    if (wasBusy && !db.busy) cheatsRefit_ = 2;
    cheatsWidth_ = ImGui::GetWindowWidth();
    std::vector<Cheat>& cheats = *state.cheats;

    if (db.files.size() > 1 || (!db.files.empty() && db.file != db.files.front())) {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Lista")).x - ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::BeginCombo(tr("Lista"), db.file.c_str())) {
            for (const std::string& f : db.files)
                if (ImGui::Selectable(f.c_str(), f == db.file)) cheatDb_.load(f);
            ImGui::EndCombo();
        }
    } else if (!db.file.empty()) {
        ImGui::TextDisabled(tr("Lista: %s"), db.file.c_str());
    }
    if (db.busy) {
        static const char* dots[] = {"", ".", "..", "..."};
        ImGui::TextDisabled(tr("Buscando en la base de trucos de libretro%s"), dots[(SDL_GetTicks() / 300) % 4]);
    }
    if (!db.status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", db.status.c_str());
    if (!db.busy && !db.status.empty() && ImGui::Button(tr("Reintentar"))) cheatDb_.search(state.cheatsPath);

    if (!db.cheats.empty()) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filtro", tr("Filtrar (lives, energy, invincible...)"), cheatFilter_, sizeof cheatFilter_);
        const std::string filter = searchKey(cheatFilter_);
        const float rowH = ImGui::GetFrameHeightWithSpacing();
        const float height = std::min(static_cast<float>(db.cheats.size()) * rowH + rowH, rowH * 9.5f);
        if (ImGui::BeginTable("trucos_libretro", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                              ImVec2(0, height))) {
            const float addW = ImGui::CalcTextSize(tr("Añadido")).x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Truco"));
            ImGui::TableSetupColumn(tr("Código"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, addW);
            ImGui::TableHeadersRow();
            int shown = 0;
            for (size_t i = 0; i < db.cheats.size(); ++i) {
                const Cheat& c = db.cheats[i];
                if (!filter.empty() && searchKey(c.name).find(filter) == std::string::npos) continue;
                ++shown;
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(c.name.empty() ? tr("(sin nombre)") : c.name.c_str());
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(c.name.c_str()).x > ImGui::GetColumnWidth())
                    ImGui::SetTooltip("%s", c.name.c_str());
                ImGui::TableNextColumn();
                std::vector<CheatPatch> patches;
                std::string error;
                const bool valid = decodeCheatCodes(c.code, decoder, &patches, &error);
                const std::string code = shortCode(c.code);
                ImGui::AlignTextToFramePadding();
                if (valid) ImGui::TextUnformatted(code.c_str());
                else ImGui::TextDisabled("%s", code.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", valid ? describePatches(patches).c_str() : error.c_str());
                ImGui::TableNextColumn();
                bool added = false;
                for (const Cheat& mine : cheats) added |= mine.code == c.code;
                if (!valid) {
                    ImGui::TextDisabled("%s", tr("No vale"));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", error.c_str());
                } else if (added) {
                    ImGui::BeginDisabled();
                    ImGui::Button(tr("Añadido"), ImVec2(-1, 0));
                    ImGui::EndDisabled();
                } else if (ImGui::Button(tr("Añadir"), ImVec2(-1, 0))) {
                    cheats.push_back({c.code, c.name, true});
                    out.cheatsChanged = true;
                    // Many games check the ROM and stop (Sonic's red screen) unless the
                    // list's master code goes with the codes that patch it.
                    bool patchesRom = false;
                    for (const CheatPatch& p : patches) patchesRom |= p.address < 0x400000;
                    for (const Cheat& m : db.cheats) {
                        if (!patchesRom || searchKey(m.name).find("master code") == std::string::npos) continue;
                        bool have = false;
                        for (const Cheat& mine : cheats) have |= mine.code == m.code;
                        if (!have) {
                            cheats.push_back({m.code, m.name, true});
                            notify(tr("Añadido también su código maestro, sin el que el juego no arrancaría"));
                        }
                    }
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", tr("Ningún truco tiene ese nombre."));
            }
            ImGui::EndTable();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("Listas de libretro-database, la base de trucos de RetroArch (licencia CC BY-SA 4.0). "
                       "Los nombres vienen en inglés; los añadidos quedan activados en la lista de arriba, "
                       "con el código maestro (Master Code) del juego cuando lo pide."));
    ImGui::PopStyleColor();
}


// The game's own console buttons, which take the place of the general ones (Opciones >
// Controles) while it is loaded. The hotkeys and the stick stay the general ones.
void Ui::drawGameControls(const GameOptionsView& v, UiActions& out) {
    std::set<std::string>& own = *v.own;
    bool mine = own.count(kGameControls) != 0;
    if (ImGui::Checkbox(tr("Botones propios de este juego"), &mine)) {
        if (mine) {
            own.insert(kGameControls);
            v.controls->copyButtons(input_.config());  // starting from the general ones
        } else {
            own.erase(kGameControls);
            if (capture_.game) capture_.active = false;
        }
        out.gameOptionsChanged = true;
    }
    if (!mine) {
        ImGui::TextDisabled("%s", tr("Usa los de Opciones > Controles."));
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Volver a los generales"))) {
        own.erase(kGameControls);
        if (capture_.game) capture_.active = false;
        out.gameOptionsChanged = true;
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Deja de usar estos y vuelve a los de Opciones > Controles"));
    ImGui::TextWrapped("%s", tr("Pulsa una casilla y luego la tecla o el botón del mando. \"+\" añade otro, "
                                "\"x\" lo borra, Esc cancela."));
    if (drawBindings(*v.controls, true)) out.gameOptionsChanged = true;
}

// The options this game keeps a value of its own for, in two tabs: Ajustes and Controles.
void Ui::drawGameOptions(const UiState& state, UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(std::min(theme::px(700), ImGui::GetMainViewport()->Size.x - 20), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(tr("Opciones de este juego"), &showGameOptions_)) {
        ImGui::End();
        return;
    }
    if (!showGameOptions_ && capture_.game) capture_.active = false;
    const GameOptionsView& v = state.gameOptions;
    if (gameControls_ != v.controls && capture_.game) capture_.active = false;  // another game
    gameControls_ = v.controls;
    if (v.game.empty() || !v.values || !v.own || !v.general) {
        ImGui::TextDisabled("%s", tr("Abre un juego, o elige uno en la biblioteca con el botón derecho."));
        ImGui::End();
        return;
    }
    ImGui::TextWrapped(tr("Opciones propias de %s. Pon una opción en Propio para que este juego use su propio valor; "
                       "las que dejes en General siguen a las generales."), v.game.c_str());
    if (v.loaded && state.netPlaying) ImGui::TextDisabled("%s", tr("En una partida en red no se pueden cambiar."));
    ImGui::BeginDisabled(v.loaded && state.netPlaying);
    if (ImGui::BeginTabBar("opciones_juego")) {
        if (ImGui::BeginTabItem(tr("Ajustes"))) {
            drawGameSettings(state, out);
            ImGui::EndTabItem();
        }
        if (v.controls && ImGui::BeginTabItem(tr("Controles"), nullptr, gameControlsTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawGameControls(v, out);
            ImGui::EndTabItem();
        }
        gameControlsTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// The Ajustes tab: each option has a switch; on ("Propio"), the game has its own value
// (edited here); off ("General"), it follows the general one (shown greyed).
void Ui::drawGameSettings(const UiState& state, UiActions& out) {
    const GameOptionsView& v = state.gameOptions;
    Settings& s = *v.values;
    std::set<std::string>& own = *v.own;
    std::string group;
    for (const GameOption& opt : gameOptions()) {
        const std::string key = opt.key;
        if (group != opt.group) {
            group = opt.group;
            ImGui::SeparatorText(tr(opt.group));
        }
        ImGui::PushID(opt.key);
        bool mine = own.count(key) != 0;
        bool edited = false;
        if (theme::switchButton("##propio", &mine, tr("Propio"), tr("General"))) {
            if (mine) own.insert(key);
            else {
                own.erase(key);
                opt.copy(*v.general, s);
            }
            edited = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mine ? tr("Propio de este juego") : tr("Usa el valor general"));
        ImGui::SameLine();
        ImGui::BeginDisabled(!mine);
        ImGui::SetNextItemWidth(220);
        if (key == "view") {
            if (ImGui::BeginCombo(tr(opt.label), s.view.c_str())) {
                for (const char* view : state.views)
                    if (ImGui::Selectable(view, s.view == view)) {
                        s.view = view;
                        edited = true;
                    }
                ImGui::EndCombo();
            }
        } else if (key == "scale") {
            static const char* names[] = {N_("Escala entera"), N_("Ajustar"), N_("Estirar")};
            int i = static_cast<int>(s.render.scale);
            if (comboTr(tr(opt.label), &i, names, 3)) {
                s.render.scale = static_cast<ScaleMode>(i);
                edited = true;
            }
        } else if (key == "smooth") {
            edited |= yesNo(tr("Suavizar píxeles"), s.render.smooth);
        } else if (key == "filter") {
            static const char* names[] = {N_("Ninguno"), N_("Scanlines"), "CRT"};
            int i = static_cast<int>(s.render.filter);
            if (comboTr(tr(opt.label), &i, names, 3)) {
                s.render.filter = static_cast<Filter2D>(i);
                edited = true;
            }
        } else if (key == "upscaling") {
            static const char* names[] = {N_("Ninguno"), "xBR", "Anime4K"};
            int i = static_cast<int>(s.render.upscale);
            if (comboTr(tr(opt.label), &i, names, 3)) {
                s.render.upscale = static_cast<Upscale2D>(i);
                edited = true;
            }
        } else if (key == "filter_strength") {
            float strength = s.render.filterStrength * 100.0f;
            if (ImGui::SliderFloat(tr(opt.label), &strength, 0.0f, 100.0f, "%.0f %%")) s.render.filterStrength = strength / 100.0f;
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "square_pixels") {
            edited |= yesNo(tr("Píxeles cuadrados (sin estirar a 4:3)"), s.render.squarePixels);
        } else if (key == "no_sprite_limit") {
            edited |= yesNo(tr("Sin límite de sprites (sin parpadeo)"), s.unlimitedSprites);
        } else if (key == "widescreen") {
            ImGui::TextUnformatted(tr(opt.label));
            edited |= widescreenControls(s.wideLeft, s.wideRight);
        } else if (key == "profile_3d") {
            ImGui::TextUnformatted(tr(opt.label));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", v.loaded ? tr("(se ajustan en Opciones > Vídeo)") : tr("(se ajustan en Vídeo con el juego abierto)"));
        } else if (key == "volume") {
            ImGui::SliderInt(tr(opt.label), &s.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "mute") {
            edited |= yesNo(tr("Silenciar"), s.mute);
        } else if (key == "region") {
            int i = static_cast<int>(s.region);
            if (comboTr(tr(opt.label), &i, kRegionNames, 4)) {
                s.region = static_cast<Region>(i);
                edited = true;
            }
        } else if (key == "pads") {
            for (size_t port = 0; port < 2; ++port) {
                if (port) ImGui::SameLine();
                const std::vector<PadType> types = portTypes(static_cast<int>(port));
                std::vector<const char*> names;
                int i = 0;
                for (size_t k = 0; k < types.size(); ++k) {
                    names.push_back(padTypeName(types[k]));
                    if (types[k] == s.padType[port]) i = static_cast<int>(k);
                }
                ImGui::SetNextItemWidth(110);
                if (comboTr(port ? tr("Mando 2") : tr("Mando 1"), &i, names.data(), static_cast<int>(names.size()))) {
                    s.padType[port] = types[static_cast<size_t>(i)];
                    edited = true;
                }
                if (ImGui::IsItemHovered()) {
                    const char* tip = padTypeTip(s.padType[port]);
                    if (!tip && port) tip = kNoPadTip;
                    if (tip) ImGui::SetTooltip("%s", tr(tip));
                }
            }
        } else if (key == "overclock") {
            edited |= overclockCombo(tr(opt.label), s.overclock);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(kOverclockTip));
        } else if (key == "rewind_seconds") {
            ImGui::SliderInt(tr(opt.label), &s.rewindSeconds, 0, Settings::kMaxRewindSeconds,
                             s.rewindSeconds ? "%d s" : tr("desactivado"), ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "fast_forward") {
            ImGui::SliderInt(tr(opt.label), &s.fastForward, 2, 10, "%dx", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "slow_motion") {
            ImGui::SliderInt(tr(opt.label), &s.slowMotion, 10, 90, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "run_ahead") {
            runAheadSlider(tr(opt.label), s.runAhead);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else {
            ImGui::TextUnformatted(tr(opt.label));  // a key with no control of its own here: at least name its box
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        if (!edited) continue;
        out.gameOptionsChanged = true;
        if (v.loaded && (key == "region" || key == "pads")) out.consoleChanged = true;
        if (v.loaded && key == "rewind_seconds") out.rewindChanged = true;
        if (v.loaded && std::string(opt.group) == "Sonido") out.soundChanged = true;
    }
}


}  // namespace plumnin
