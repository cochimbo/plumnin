#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace plumnin {

enum class DialogResult { Chosen, Cancelled, Unavailable };
// What is being picked: a game (cartridge or disc), a folder, any file (a BIOS) or an
// image (a cover).
enum class Pick { Game, Folder, AnyFile, Image };

// The system "open file" dialog, filtered to games (Pick::Game), to images (Pick::Image) or showing every file
// (Pick::AnyFile). Blocks until closed. Returns Unavailable on platforms without one;
// the caller then uses FileBrowser.
DialogResult openFileDialog(SDL_Window* window, const std::string& startFolder, Pick pick, std::string* chosen);

// Opens a file or folder with what the system uses for it (Explorer, the Linux desktop's file manager).
void openInSystem(const std::string& path);

// File picker drawn with Dear ImGui: for platforms without a native dialog, for full
// screen (where the system dialog can open hidden behind the game) and to choose the
// library's folders and the BIOS files. Mouse, keyboard or pad: up and down to move, A (or Enter) to open,
// B (or Backspace) to go up a folder, Esc to close.
class FileBrowser {
public:
    // Picks a game, a folder ("Usar esta carpeta") or any file.
    void open(const std::string& folder, Pick pick = Pick::Game);
    bool isOpen() const { return open_; }
    Pick picking() const { return pick_; }
    // Keys and pad buttons while it is open; true if it used the event.
    bool handleEvent(const SDL_Event& e);
    // Call every frame between ImGui::NewFrame and ImGui::Render. Returns true once a
    // file (or folder) has been picked, with its path in *chosen.
    bool draw(std::string* chosen);

private:
    struct Entry {
        std::string name;
        bool folder;
    };
    void list(const std::string& folder);
    void up();

    bool open_ = false;
    bool justOpened_ = false;
    Pick pick_ = Pick::Game;
    bool drives_ = false;  // listing the drives (Windows), above every folder
    std::string folder_;
    std::vector<Entry> entries_;
    int selected_ = 0;
    bool scroll_ = false;
    bool activate_ = false;  // A / Enter pressed on the selection
};

}  // namespace plumnin
