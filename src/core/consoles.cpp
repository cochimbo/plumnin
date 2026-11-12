#include "console.h"
#include "gb/gb.h"
#include "i18n.h"

namespace plumnin {

// The consoles are added here as they are emulated: the Game Boy and Game Boy Color
// first, then the NES, then the Super Nintendo.
namespace {

bool gameBoy(ConsoleSystem s) { return s == ConsoleSystem::GameBoy || s == ConsoleSystem::GameBoyColor; }

}  // namespace

bool emulated(ConsoleSystem s) { return gameBoy(s); }

std::unique_ptr<Console> createConsole(ConsoleSystem s) {
    if (gameBoy(s)) return std::make_unique<GameBoy>();
    return nullptr;
}

CheatDecoder cheatDecoderFor(ConsoleSystem s) { return gameBoy(s) ? GameBoy::decoder() : CheatDecoder{}; }

const char* cheatFormatsFor(ConsoleSystem s) { return gameBoy(s) ? GameBoy::cheatFormats() : ""; }

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
