#pragma once

#include <SDL.h>

#include <array>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "../core/pad.h"

namespace plumnin {

// The console buttons that can be bound: the Super Nintendo pad's, which has them all.
enum class PadButton { Up, Down, Left, Right, A, B, X, Y, L, R, Select, Start, Count };

// Emulator actions that can be bound like console buttons. Most fire once per press;
// Rewind and FastForward last while held.
enum class Hotkey {
    Toggle3D, SaveState, LoadState, PrevSlot, NextSlot, StateMenu, Rewind, FastForward, SlowMotion, Library, Stop, FpsCounter, Count
};

// SDL reports the pad's triggers as axes; bindings keep them as two extra buttons.
constexpr auto kPadLeftTrigger = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX);
constexpr auto kPadRightTrigger = static_cast<SDL_GameControllerButton>(SDL_CONTROLLER_BUTTON_MAX + 1);
// Names as in the [mando] section: SDL's button names plus lefttrigger / righttrigger.
const char* padButtonName(SDL_GameControllerButton b);
SDL_GameControllerButton padButtonFromName(const std::string& name);  // INVALID if unknown
// The trigger a pad axis event pulls past halfway, or SDL_CONTROLLER_BUTTON_INVALID.
SDL_GameControllerButton triggerPulled(const SDL_Event& e);

// Keyboard and gamepad bindings, read from and written to a plain-text .ini file.
// The keyboard can drive both players ([teclado] and [teclado2]); the first gamepad
// is player 1 and the second player 2, both with the [mando] buttons.
struct InputConfig {
    static constexpr int kButtons = static_cast<int>(PadButton::Count);
    static constexpr int kHotkeys = static_cast<int>(Hotkey::Count);
    std::array<std::vector<SDL_Scancode>, kButtons> keys;
    std::array<std::vector<SDL_Scancode>, kButtons> keys2;
    std::array<std::vector<SDL_GameControllerButton>, kButtons> pad;
    std::array<std::vector<SDL_Scancode>, kHotkeys> hotkeyKeys;
    std::array<std::vector<SDL_GameControllerButton>, kHotkeys> hotkeyPad;
    bool stickAsDpad = true;
    int stickDeadzone = 12000;

    static InputConfig defaults();
    // Loads `path`; if it does not exist, writes the defaults there so the user can edit them.
    static InputConfig loadOrCreate(const std::string& path);
    // Applies the [teclado], [teclado2] and [mando] keys in `in` onto `c`. Returns whether
    // any console button was among them.
    static bool parse(std::istream& in, InputConfig& c);
    bool save(const std::string& path) const;
    // Writes the [teclado] and [mando] sections, so other settings can share the file.
    void write(std::ostream& out) const;
    // Only the console buttons of the three sections (no hotkeys or stick), for a game's file.
    void writeButtons(std::ostream& out) const;
    // Takes the console buttons (both keyboards and the pad) from `from`.
    void copyButtons(const InputConfig& from);
};

// Tracks connected controllers and turns the keyboard and pads into both players' pads.
class Input {
public:
    explicit Input(InputConfig config) : config_(std::move(config)) {}
    ~Input();

    void openAll();
    void handleEvent(const SDL_Event& e);
    // `useKeyboard` = false ignores the keyboard (e.g. while a menu has focus).
    PadState poll(int player, bool useKeyboard = true) const;
    // Leaves out of poll() the console buttons held right now, each until it is let go:
    // the press that closed a menu (loading or saving a state) must not reach the game.
    void ignoreHeld();
    // The hotkey this key or pad button press triggers, or Hotkey::Count if none.
    Hotkey hotkeyFor(const SDL_Event& e) const;
    // Whether a key or pad button bound to `h` is down right now.
    bool held(Hotkey h, bool useKeyboard = true) const;
    // Right stick of the first pad that has it pushed past the dead zone, each axis -1..1
    // (y positive down); 0,0 when none is. Turns the pseudo-3D camera.
    void rightStick(float* x, float* y) const;
    // The left stick of the first pad that has it pushed, the same way: turns what the
    // library's 3D view has out in front.
    void leftStick(float* x, float* y) const;
    // The left stick of the player's pad the same way, for a light gun's crosshair.
    void leftStick(int player, float* x, float* y) const;

    // The general bindings, as in plumnin.ini.
    const InputConfig& config() const { return config_; }
    void setConfig(InputConfig config) { config_ = std::move(config); }
    // A game's own console buttons, used instead of the general ones while it is loaded;
    // nullptr goes back to the general ones. Hotkeys and the stick always stay general.
    void setGameButtons(const InputConfig* buttons);
    bool hasGameButtons() const { return gameButtons_.has_value(); }
    std::vector<std::string> padNames() const;

private:
    // The first pad's stick (SDL's axes) pushed past the dead zone; 0,0 when none is.
    void firstStick(int axisX, int axisY, float* x, float* y) const;
    std::array<bool, InputConfig::kButtons> buttonsDown(int player, bool useKeyboard) const;

    InputConfig config_;
    std::optional<InputConfig> gameButtons_;
    std::vector<SDL_GameController*> pads_;
    // Triggers held on each pad (by joystick id), to see a pull as a single press.
    std::vector<std::pair<SDL_JoystickID, int>> triggers_;  // bit 0 left, bit 1 right
    SDL_GameControllerButton pulled_ = SDL_CONTROLLER_BUTTON_INVALID;  // by the last event
    // Per player, the buttons ignoreHeld() left out that have not been let go yet.
    mutable std::array<std::array<bool, InputConfig::kButtons>, 2> ignored_{};
};

}  // namespace plumnin
