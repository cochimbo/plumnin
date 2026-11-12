#include "osk.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // BringWindowToDisplayFront
#include "theme.h"
#include "../core/i18n.h"

namespace plumnin {

namespace {

// Ten keys a row; the second string is with Mayúsculas. Enough for a search, an
// address (192.168.1.20:7845), a cheat code or an e-mail.
const char* const kKeys[4][2][OnScreenKeyboard::kColumns] = {
    {{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}, {"!", "@", "#", "$", "%", "&", "/", "(", ")", "="}},
    {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}, {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"}},
    {{"a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB1"}, {"A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x91"}},
    {{"z", "x", "c", "v", "b", "n", "m", ",", ".", "-"}, {"Z", "X", "C", "V", "B", "N", "M", ";", ":", "_"}},
};

// A key pressed and let go at once: ImGui spreads the two over frames by itself.
void tap(ImGuiKey key) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
}

}  // namespace

void OnScreenKeyboard::open() {
    if (open_) return;
    open_ = true;
    row_ = 1;
    column_ = 0;
    shift_ = false;
    heldX_ = heldY_ = 0;
}

void OnScreenKeyboard::step(int& row, int& column, int dx, int dy) {
    const int bottom = kRows - 1;
    if (dy != 0) {
        // The bottom row's four keys are each two and a half letters wide.
        if (row == bottom) column = std::min(kColumns - 1, column * kColumns / kBottomKeys + 1);
        row = (row + dy + kRows) % kRows;
        if (row == bottom) column = column * kBottomKeys / kColumns;
    }
    if (dx != 0) {
        const int count = row == bottom ? kBottomKeys : kColumns;
        column = (column + dx + count) % count;
    }
}

const char* OnScreenKeyboard::keyText(int row, int column, bool shift) {
    if (row < 0 || row >= 4 || column < 0 || column >= kColumns) return "";
    return kKeys[row][shift ? 1 : 0][column];
}

void OnScreenKeyboard::press() {
    ImGuiIO& io = ImGui::GetIO();
    if (row_ < kRows - 1) {
        io.AddInputCharactersUTF8(keyText(row_, column_, shift_));
        return;
    }
    switch (column_) {
        case 0: shift_ = !shift_; break;
        case 1: io.AddInputCharacter(' '); break;
        case 2: tap(ImGuiKey_Backspace); break;
        default: tap(ImGuiKey_Enter); break;
    }
}

bool OnScreenKeyboard::handleEvent(const SDL_Event& e) {
    if (!open_) return false;
    if (e.type == SDL_CONTROLLERBUTTONUP) {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: heldX_ = 0; break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_DOWN: heldY_ = 0; break;
            default: break;
        }
        return true;
    }
    if (e.type == SDL_CONTROLLERAXISMOTION) return true;
    if (e.type != SDL_CONTROLLERBUTTONDOWN) return false;
    int dx = 0, dy = 0;
    switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dx = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dx = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: dy = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dy = 1; break;
        case SDL_CONTROLLER_BUTTON_A: press(); break;
        case SDL_CONTROLLER_BUTTON_B: tap(ImGuiKey_Backspace); break;
        case SDL_CONTROLLER_BUTTON_Y: ImGui::GetIO().AddInputCharacter(' '); break;
        case SDL_CONTROLLER_BUTTON_X: shift_ = !shift_; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: tap(ImGuiKey_LeftArrow); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: tap(ImGuiKey_RightArrow); break;
        case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_BACK: tap(ImGuiKey_Enter); break;
        default: break;
    }
    if (dx || dy) {
        step(row_, column_, dx, dy);
        if (dx) heldX_ = dx;
        if (dy) heldY_ = dy;
        repeatAt_ = SDL_GetTicks() + 400;
    }
    return true;
}

void OnScreenKeyboard::draw() {
    if (!open_) return;
    if ((heldX_ || heldY_) && SDL_GetTicks() >= repeatAt_) {
        step(row_, column_, heldX_, heldY_);
        repeatAt_ = SDL_GetTicks() + 90;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float key = std::min(theme::px(44.0f), (vp->Size.x - theme::px(40.0f)) / kColumns);
    const float gap = theme::px(4.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = key * kColumns + gap * (kColumns - 1) + style.WindowPadding.x * 2;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - theme::px(8.0f)),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    // Never takes the focus: the text box being typed in has to stay active.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove;
    ImGui::Begin("##teclado", nullptr, flags);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImU32 face = ImGui::GetColorU32(ImGuiCol_Button), lit = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), border = ImGui::GetColorU32(ImGuiCol_Border);
    auto drawKey = [&](ImVec2 at, float w, const char* label, bool chosen, bool on) {
        const ImVec2 to(at.x + w, at.y + key);
        d->AddRectFilled(at, to, chosen ? lit : on ? ImGui::GetColorU32(ImGuiCol_Header) : face);
        d->AddRect(at, to, chosen ? text : border);
        const ImVec2 size = ImGui::CalcTextSize(label);
        d->AddText(ImVec2(at.x + (w - size.x) * 0.5f, at.y + (key - size.y) * 0.5f), text, label);
    };
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int r = 0; r < kRows - 1; ++r)
        for (int c = 0; c < kColumns; ++c)
            drawKey(ImVec2(origin.x + c * (key + gap), origin.y + r * (key + gap)), key, keyText(r, c, shift_),
                    r == row_ && c == column_, false);
    const char* const bottom[kBottomKeys] = {tr("Mayúsculas"), tr("Espacio"), tr("Borrar"), tr("Aceptar")};
    const float wide = (key * kColumns + gap * (kColumns - 1) - gap * (kBottomKeys - 1)) / kBottomKeys;
    for (int c = 0; c < kBottomKeys; ++c)
        drawKey(ImVec2(origin.x + c * (wide + gap), origin.y + (kRows - 1) * (key + gap)), wide, bottom[c],
                row_ == kRows - 1 && c == column_, c == 0 && shift_);
    ImGui::Dummy(ImVec2(key * kColumns + gap * (kColumns - 1), kRows * key + (kRows - 1) * gap));
    theme::hint(tr("A: escribir   B: borrar   Y: espacio   X: mayúsculas\nL1 / R1: mover el cursor   Start: aceptar"));
    ImGui::End();
}

}  // namespace plumnin
