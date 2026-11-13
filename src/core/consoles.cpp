#include "console.h"
#include "gb/gb.h"
#include "nes/nes.h"
#include "i18n.h"

namespace plumnin {

// The consoles are added here as they are emulated: the Game Boy and Game Boy Color
// first, then the NES, then the Super Nintendo.
namespace {

bool gameBoy(ConsoleSystem s) { return s == ConsoleSystem::GameBoy || s == ConsoleSystem::GameBoyColor; }

}  // namespace

bool emulated(ConsoleSystem s) { return gameBoy(s) || s == ConsoleSystem::Nes; }

std::unique_ptr<Console> createConsole(ConsoleSystem s) {
    if (gameBoy(s)) return std::make_unique<GameBoy>();
    if (s == ConsoleSystem::Nes) return std::make_unique<Nes>();
    return nullptr;
}

CheatDecoder cheatDecoderFor(ConsoleSystem s) {
    if (gameBoy(s)) return GameBoy::decoder();
    if (s == ConsoleSystem::Nes) return Nes::decoder();
    return {};
}

const char* cheatFormatsFor(ConsoleSystem s) {
    if (gameBoy(s)) return GameBoy::cheatFormats();
    if (s == ConsoleSystem::Nes) return Nes::cheatFormats();
    return "";
}

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
