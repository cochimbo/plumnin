#include "handheld.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace plumnin {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

// The computer's model: what its firmware says (product name and family, one after the other).
std::string machineModel() {
    std::string model;
#ifdef _WIN32
    for (const char* value : {"SystemProductName", "SystemFamily", "SystemManufacturer"}) {
        char buffer[256] = {};
        DWORD size = sizeof(buffer);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", value, RRF_RT_REG_SZ, nullptr,
                         buffer, &size) == ERROR_SUCCESS)
            model += std::string(buffer) + " ";
    }
#else
    for (const char* file : {"/sys/class/dmi/id/product_name", "/sys/class/dmi/id/product_family",
                             "/sys/class/dmi/id/sys_vendor"}) {
        std::ifstream in(file);
        std::string line;
        if (std::getline(in, line)) model += line + " ";
    }
#endif
    return model;
}

}  // namespace

std::string handheldFromModel(const std::string& model) {
    // Lower-case pieces of the firmware's names, and what to call each machine.
    static const struct {
        const char* piece;
        const char* name;
    } kKnown[] = {
        {"jupiter", "Steam Deck"},       // the LCD Steam Deck's board
        {"galileo", "Steam Deck"},       // the OLED one's
        {"steam deck", "Steam Deck"},
        {"rog ally", "ROG Ally"},        // "ROG Ally RC71L", "ROG Ally X RC72LA"
        {"rc71l", "ROG Ally"},
        {"rc72l", "ROG Ally"},
        {"rog xbox ally", "ROG Xbox Ally"},
        {"rc73", "ROG Xbox Ally"},
        {"claw", "MSI Claw"},            // "Claw A1M", "Claw 8 AI+"...
        {"legion go", "Legion Go"},      // its family: "Legion Go 8APU1"
        {"83e1", "Legion Go"},           // its product number
        {"83n0", "Legion Go S"},
        {"83n1", "Legion Go S"},
        {"83l3", "Legion Go S"},
        {"aya neo", "AYANEO"},
        {"ayaneo", "AYANEO"},
        {"onexplayer", "OneXPlayer"},
        {"onexfly", "OneXFly"},
        {"gpd win", "GPD Win"},
        {"zotac gaming zone", "Zotac Zone"},
        {"ayn loki", "AYN Loki"},
    };
    const std::string m = lower(model);
    for (const auto& k : kKnown)
        if (m.find(k.piece) != std::string::npos) return k.name;
    return {};
}

const std::string& consoleModeReason() {
    static const std::string reason = [] {
        // Steam's game mode (gamescope) and Big Picture say so to the games they start;
        // a Steam Deck also says it is one, under Proton too.
        if (envSet("SteamDeck")) return std::string("Steam Deck");
        if (envSet("SteamGamepadUI") || envSet("SteamTenfoot")) return std::string("Steam Big Picture");
        return handheldFromModel(machineModel());
    }();
    return reason;
}

}  // namespace plumnin
