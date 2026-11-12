#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace plumnin {

// One write a cheat code asks for, in the console's own addresses. Where it lands is
// the console's business: a Game Genie patches what the cartridge's ROM answers, a Pro
// Action Replay (or GameShark) writes RAM again every frame.
struct CheatPatch {
    uint32_t address = 0;
    uint16_t value = 0;
    bool byte = true;  // write only the low byte of value
    // The byte must be this for the patch to apply (-1: always). The Game Genies of the
    // Game Boy and the NES check it, so that a patch meant for one bank leaves the
    // others alone.
    int compare = -1;
    bool ram = false;  // written to RAM every frame rather than patching the ROM
};

// A code as the user typed it, with a name and whether it is on.
struct Cheat {
    std::string code;
    std::string name;
    bool enabled = true;
};

// Turns one code (already in upper case, without spaces) into its patch: each console
// knows its own codes. False, with a message in Spanish, if the code is not one.
using CheatDecoder = std::function<bool(const std::string& code, CheatPatch* out, std::string* error)>;

// A code made of several joined with '+' ("ABCD-EFGH+7E0DBF:09"), the way cheat lists
// write a cheat that needs more than one write: all its writes, or false with the
// message of the first part that is wrong. Without a decoder every code is wrong.
bool decodeCheatCodes(const std::string& code, const CheatDecoder& decode, std::vector<CheatPatch>* out,
                      std::string* error);
// The code in upper case without spaces, the way the cheat list shows it.
std::string normalizeCheat(const std::string& code);

// A game's cheat file: one line per code, "si" or "no", the code and an optional name.
std::string formatCheats(const std::vector<Cheat>& cheats);
std::vector<Cheat> parseCheats(const std::string& text);
// The cheats of a RetroArch .cht file (the libretro-database lists): each cheatN_code
// with cheatN_desc as its name, all off. Entries with no code are left out.
std::vector<Cheat> parseChtFile(const std::string& text);

}  // namespace plumnin
