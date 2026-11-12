#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumnin {

// The consoles plumnin is made for. The Game Boy Color also runs the Game Boy's
// cartridges, and the Game Boy the ones the Color made that still work on it.
enum class ConsoleSystem : uint8_t { GameBoy = 0, GameBoyColor = 1, Nes = 2, Snes = 3 };
constexpr int kSystemCount = 4;

// Which television the console is made for. The Game Boy has none: it ignores it.
enum class Region : uint8_t { Auto = 0, JapanNtsc = 1, UsaNtsc = 2, EuropePal = 3 };

// "Game Boy", "Game Boy Color", "NES", "Super Nintendo".
const char* systemName(ConsoleSystem s);
// The same without spaces, for file and folder names: "GameBoy", "GameBoyColor", "NES", "SNES".
const char* systemKey(ConsoleSystem s);

// The extensions of each console's cartridges.
bool hasRomExtension(const std::string& path);
// True for the files plumnin can open: cartridges (.gb .gbc .nes .sfc .smc) and a .zip
// with one inside.
bool isGameFile(const std::string& path);

// Which console a game file is for: by extension, and for a .zip by the cartridge in it.
// A .gb whose header says it is made for the Color is a Game Boy Color game.
ConsoleSystem detectSystem(const std::string& path);
// The same from the cartridge itself, when the name says nothing: the NES's "NES\x1A"
// header, the Game Boy's logo at 0x104, else the SNES.
ConsoleSystem detectSystemFromData(const std::vector<uint8_t>& rom);

// Reads a cartridge, or the first cartridge inside a .zip, into `rom`, and its name
// inside the zip (the file's own otherwise) into `name`. False, with the reason in
// Spanish, if it cannot.
bool readRomFile(const std::string& path, std::vector<uint8_t>& rom, std::string* name, std::string* error);

}  // namespace plumnin
