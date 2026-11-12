#include "console.h"
#include "i18n.h"

namespace plumnin {

// The consoles are added here as they are emulated: the Game Boy and Game Boy Color
// first, then the NES, then the Super Nintendo.
bool emulated(ConsoleSystem) { return false; }

std::unique_ptr<Console> createConsole(ConsoleSystem) { return nullptr; }

CheatDecoder cheatDecoderFor(ConsoleSystem) { return {}; }

const char* cheatFormatsFor(ConsoleSystem) { return ""; }

std::unique_ptr<Console> openGame(const std::string& path, std::string* error) {
    const ConsoleSystem system = detectSystem(path);
    std::unique_ptr<Console> console = createConsole(system);
    if (!console) {
        if (error) *error = trf("plumnin todavía no emula la %s.", systemName(system));
        return nullptr;
    }
    if (!console->loadRom(path, error)) return nullptr;
    return console;
}

}  // namespace plumnin
