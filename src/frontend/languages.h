#pragma once

#include <string>

namespace plumnin {

// The languages plumnin speaks, with their names as their speakers write them.
struct LanguageInfo {
    const char* code;  // "es", "en", "fr", "pt"
    const char* name;  // "Español", "English"...
};
constexpr LanguageInfo kLanguages[] = {
    {"es", "Español"},
    {"en", "English"},
    {"fr", "Français"},
    {"pt", "Português"},
};

// The first of the system's preferred languages that plumnin has; English if none is.
std::string systemLanguage();
// Makes tr() speak `code` (one of kLanguages; anything else is Spanish).
void applyLanguage(const std::string& code);

}  // namespace plumnin
