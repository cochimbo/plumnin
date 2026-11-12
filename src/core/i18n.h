#pragma once

#include <string>

namespace plumnin {

// The texts people read are written in Spanish in the code; tr() gives them in the
// language chosen with setLanguage(), or as they are if it has no translation for them.
// The translations live in src/frontend/lang/<code>.po, one entry per Spanish text, and
// tools/i18n.py keeps those files in step with the code (see src/frontend/lang/README.md).
//
// A label's "##id" part is kept as it is and only the text before it is translated.
// The pointer stays valid for as long as the program runs.
const char* tr(const char* spanish);
// tr() of a printf format, filled in: trf("Ranura %d", n). The translation keeps the
// same %-specifiers in the same order.
std::string trf(const char* spanish, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
// Marks a text that is translated later, where it is used (in an array, say), so the
// tools still find it: kNames[] = {N_("Arriba"), ...} and then tr(kNames[i]).
constexpr const char* N_(const char* spanish) { return spanish; }

// The language tr() translates to: "es" (the texts as written) or one whose .po text
// is given. Thread-safe; texts already handed out stay valid.
void setLanguage(const std::string& code, const std::string& poText = {});
std::string language();

}  // namespace plumnin
