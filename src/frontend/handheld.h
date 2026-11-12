// Console mode: is plumnin being played from the sofa, with a pad and no keyboard? On a
// Steam Deck, in Steam's game mode or Big Picture, and on the Windows handhelds (ROG Ally,
// MSI Claw, Legion Go...), which Steam does not start.
#pragma once

#include <string>

namespace plumnin {

// The handheld a computer's model name (its firmware's product name or family) is, as
// shown to the user ("ROG Ally"), or empty if it is not one known.
std::string handheldFromModel(const std::string& model);

// What makes console mode turn on by itself here: the handheld, "Steam Big Picture"...
// Empty on a normal PC. Looked at once and remembered.
const std::string& consoleModeReason();

}  // namespace plumnin
