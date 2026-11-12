// plumnin's look, inherited from plumsg: a console as an object. Black ribbed plastic, a
// red line, chrome italic lettering and round pad buttons, on top of Dear ImGui's widgets.
#pragma once

#include "imgui.h"

namespace theme {

// The typefaces, Chakra Petch (OFL, built into the executable). Null until addFonts.
struct Fonts {
    ImFont* body = nullptr;   // everything
    ImFont* title = nullptr;  // bold italic: window titles, a game's name, the main button
};
const Fonts& fonts();

// Add each font to the atlas at `size` pixels, the characters in `ranges`; the
// system's fonts for other scripts can be merged into it right after.
void addBodyFont(float size, const ImWchar* ranges);
void addTitleFont(float size, const ImWchar* ranges);

// The colours and shapes, after ImGui::CreateContext and again when the size changes:
// the style is made anew at `scale` times its size (1 is 100 %).
void apply(float scale = 1.0f);
// The interface's size last applied, and `pixels` at that size, for the sizes of windows.
float scale();
inline float px(float pixels) { return pixels * scale(); }

// Repaints the title bars of the windows drawn this frame (ribs, red line, chrome
// title). Between the last End and ImGui::Render.
void decorateWindows();

// The one action a window is for ("Jugar"): a slanted red button, `width` wide
// (-1 for all the room left).
bool primaryButton(const char* label, float width = -1.0f);

// A two-way switch drawn as one plate: `on` lit red with `onLabel`, off dark with
// `offLabel`. Clicking it flips `*on` and returns true.
bool switchButton(const char* id, bool* on, const char* onLabel, const char* offLabel);

// A white grid on black, as on some 80s game boxes, over `min`-`max` of the current
// window, moving with its scroll. Before the items drawn on it.
void boxGrid(ImVec2 min, ImVec2 max);

// A line of controls help ("A / Intro: jugar   B / Esc: cerrar"), dim, with the pad's
// buttons drawn as buttons. Several lines with '\n'.
void hint(const char* text);

}  // namespace theme
