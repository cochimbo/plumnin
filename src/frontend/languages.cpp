#include "languages.h"

#include <SDL.h>

#include "lang_po.h"
#include "../core/i18n.h"

namespace plumnin {

std::string systemLanguage() {
    std::string found;
    if (SDL_Locale* locales = SDL_GetPreferredLocales()) {
        for (const SDL_Locale* l = locales; l->language && found.empty(); ++l)
            for (const LanguageInfo& lang : kLanguages)
                if (SDL_strcasecmp(l->language, lang.code) == 0) found = lang.code;
        SDL_free(locales);
    }
    return found.empty() ? "en" : found;
}

void applyLanguage(const std::string& code) {
    auto text = [](const unsigned char* po, size_t size) { return std::string(reinterpret_cast<const char*>(po), size); };
    if (code == "en") setLanguage(code, text(kLangPo_en, sizeof kLangPo_en));
    else if (code == "fr") setLanguage(code, text(kLangPo_fr, sizeof kLangPo_fr));
    else if (code == "pt") setLanguage(code, text(kLangPo_pt, sizeof kLangPo_pt));
    else setLanguage("es");
}

}  // namespace plumnin
