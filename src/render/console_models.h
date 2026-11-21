#pragma once

#include <vector>

#include "../core/system.h"

namespace plumnin {

// The consoles the library's 3D view puts the cartridges into, with 13.5 cm (the width of
// a Mega Drive case, as in plumsg) as 1. Each model stands on the floor with the middle of
// its footprint at the origin and its front towards +z. These are the built-in ones, simple
// shapes of the right size, drawn only when the console's glTF model (assets/models) is
// missing; the places below are the same on both.
enum class ConsoleModel {
    GameBoy,       // the first model (DMG-01): 90 x 148 x 32 mm, standing up facing the viewer
    GameBoyColor,  // 78 x 133.5 x 27.4 mm, standing up facing the viewer
    Nes,           // the front-loading NES: 254 x 203 x 85 mm
    Snes,          // the European and Japanese Super Nintendo: 200 x 242 x 72 mm
    Count
};

// Where things are on the models, for placing the cartridges and the lights.
struct ConsoleLayout {
    // Game Boy: the slot along its top, at the back; its screen on the front (middle and
    // size) and the power light left of it.
    static constexpr float kGbSlot[3] = {-0.009f, 1.104f, -0.078f};
    static constexpr float kGbScreen[4] = {-0.005f, 0.793f, 0.395f, 0.356f};
    static constexpr float kGbFrontZ = 0.108f;
    static constexpr float kGbLed[3] = {-0.247f, 0.841f, 0.114f};
    // Game Boy Color, the same.
    static constexpr float kGbcSlot[3] = {0.0f, 0.956f, -0.069f};
    static constexpr float kGbcScreen[4] = {-0.001f, 0.762f, 0.315f, 0.287f};
    static constexpr float kGbcFrontZ = 0.097f;
    static constexpr float kGbcLed[3] = {-0.214f, 0.797f, 0.096f};
    // NES: the middle of the door's mouth at the front, where the cartridge goes in lying
    // on its back, and how far in and down it goes from there; its power light.
    static constexpr float kNesDoor[3] = {-0.209f, 0.469f, 0.647f};
    static constexpr float kNesIn = 0.54f, kNesDown = 0.07f;
    static constexpr float kNesLed[3] = {-0.714f, 0.154f, 0.497f};
    static constexpr float kNesHeight = 0.578f;
    // Super Nintendo: the slot on its top, its power light at the front.
    static constexpr float kSnesSlot[3] = {0.0f, 0.488f, -0.167f};
    static constexpr float kSnesLed[3] = {0.39f, 0.437f, 0.774f};
    static constexpr float kSnesHeight = 0.508f;
};

// Appends the model's triangles: per vertex its position, normal, texture coordinates
// (unused, 0), face (0) and colour, kConsoleVertexFloats floats.
constexpr int kConsoleVertexFloats = 12;
void buildConsoleModel(ConsoleModel model, std::vector<float>& out);

// The console a system's games go into: the Game Boy Color's own games into a Color, the
// rest of the Game Boy's into a Game Boy.
ConsoleModel consoleModelOf(ConsoleSystem s);

// A console's cartridge, from measured ones: its size, where its label is on the front
// (from its middle), how deep it goes into the slot and the slot's width.
struct CartridgeShape {
    float w, h, d;
    float labelW, labelH, labelY;
    float insert;
    float slotW;
};
CartridgeShape cartridgeShape(ConsoleSystem s);
// Appends the cartridge's shell and contacts (not its label), centred on the origin with
// the label's side towards +z, in the same layout as buildConsoleModel.
void buildCartridgeModel(ConsoleSystem s, std::vector<float>& out);

}  // namespace plumnin
