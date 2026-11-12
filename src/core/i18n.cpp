#include "i18n.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>

namespace plumnin {

namespace {

// One language's translations. Labels with an "##id" are put together once and kept,
// so the pointers tr() hands out never move.
struct Catalog {
    std::map<std::string, std::string, std::less<>> texts;
    std::map<std::string, std::string, std::less<>> labels;
};

std::mutex mutex;
std::map<std::string, Catalog> catalogs;  // every language loaded so far, never freed
Catalog* current = nullptr;               // null: Spanish
std::string currentCode = "es";

// The text of a PO quoted string, without its quotes and with its escapes undone.
std::string unquote(std::string_view s) {
    const size_t a = s.find('"'), b = s.rfind('"');
    if (a == std::string_view::npos || b <= a) return {};
    std::string out;
    for (size_t i = a + 1; i < b; ++i) {
        if (s[i] != '\\' || i + 1 >= b) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default: out += s[i]; break;  // \" and \\ and the rest
        }
    }
    return out;
}

// msgid / msgstr pairs; entries marked fuzzy or left empty are not translations yet.
void parsePo(const std::string& text, Catalog& catalog) {
    std::istringstream in(text);
    std::string line, id, str;
    std::string* field = nullptr;
    bool fuzzy = false, nextFuzzy = false;
    auto flush = [&] {
        if (!id.empty() && !str.empty() && !fuzzy) catalog.texts[id] = str;
        id.clear();
        str.clear();
        field = nullptr;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#,", 0) == 0) {
            nextFuzzy = line.find("fuzzy") != std::string::npos;
        } else if (line.rfind("msgid ", 0) == 0) {
            flush();
            fuzzy = nextFuzzy;
            nextFuzzy = false;
            id = unquote(line);
            field = &id;
        } else if (line.rfind("msgstr ", 0) == 0) {
            str = unquote(line);
            field = &str;
        } else if (!line.empty() && line[0] == '"' && field) {
            *field += unquote(line);
        }
    }
    flush();
}

}  // namespace

const char* tr(const char* spanish) {
    if (!spanish || !*spanish) return spanish;
    std::lock_guard<std::mutex> lock(mutex);
    if (!current) return spanish;
    const std::string_view s(spanish);
    const size_t hash = s.find("##");
    if (hash == std::string_view::npos) {
        auto it = current->texts.find(s);
        return it == current->texts.end() ? spanish : it->second.c_str();
    }
    auto label = current->labels.find(s);
    if (label != current->labels.end()) return label->second.c_str();
    auto it = current->texts.find(s.substr(0, hash));
    std::string text = it == current->texts.end() ? std::string(s.substr(0, hash)) : it->second;
    text += s.substr(hash);
    return current->labels.emplace(std::string(s), std::move(text)).first->second.c_str();
}

std::string trf(const char* spanish, ...) {
    const char* format = tr(spanish);
    va_list args, copy;
    va_start(args, spanish);
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&out[0], out.size(), format, args);
        out.resize(static_cast<size_t>(n));
    }
    va_end(args);
    return out;
}

void setLanguage(const std::string& code, const std::string& poText) {
    std::lock_guard<std::mutex> lock(mutex);
    currentCode = code.empty() ? "es" : code;
    if (currentCode == "es") {
        current = nullptr;
        return;
    }
    auto found = catalogs.find(currentCode);
    if (found == catalogs.end()) {
        found = catalogs.emplace(currentCode, Catalog{}).first;
        parsePo(poText, found->second);
    }
    current = &found->second;
}

std::string language() {
    std::lock_guard<std::mutex> lock(mutex);
    return currentCode;
}

}  // namespace plumnin
