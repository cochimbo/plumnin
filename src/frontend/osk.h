// The on-screen keyboard of console mode: for typing in a text box with the pad alone
// (the library's search, a host's address, a cheat code...), on a Steam Deck, a ROG Ally
// or any PC without a keyboard at hand. It types into whatever ImGui text box is active.
#pragma once

#include <SDL.h>

namespace plumnin {

class OnScreenKeyboard {
public:
    // Rows of keys: the letters' four of ten keys, then Mayúsculas, Espacio, Borrar, Aceptar.
    static constexpr int kColumns = 10, kRows = 5, kBottomKeys = 4;

    bool isOpen() const { return open_; }
    void open();
    void close() { open_ = false; }
    // While open it owns the pad: the d-pad moves, A types, B rubs out, Y a space, X
    // capitals, L1 / R1 move the text's cursor and Start (or Select) accepts. True if used.
    bool handleEvent(const SDL_Event& e);
    // At the bottom of the screen, after the windows; repeats the d-pad held down.
    void draw();

    // Which key the d-pad lands on from (row, column) going (dx, dy), for tests.
    static void step(int& row, int& column, int dx, int dy);
    // What the key at (row, column) of the letters types (UTF-8), with or without capitals.
    static const char* keyText(int row, int column, bool shift);

private:
    void press();

    bool open_ = false;
    int row_ = 1, column_ = 0;
    bool shift_ = false;
    int heldX_ = 0, heldY_ = 0;  // the d-pad held down, to repeat it
    Uint32 repeatAt_ = 0;
};

}  // namespace plumnin
