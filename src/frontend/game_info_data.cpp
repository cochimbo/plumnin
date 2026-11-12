#include "game_info_data.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <set>

#include "../core/i18n.h"
#include "library_names.h"

namespace plumnin {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// A quoted string at `i` (just after the quote) up to the closing one; no escapes in
// clrmamepro files.
std::string quoted(const std::string& text, size_t i, size_t lineEnd) {
    size_t end = text.find('"', i);
    if (end == std::string::npos || end > lineEnd) end = lineEnd;
    return text.substr(i, end - i);
}

// The value after a key on the line from `i` to `lineEnd` (the key already read):
// quoted or a word.
std::string lineValue(const std::string& text, size_t i, size_t lineEnd) {
    while (i < lineEnd && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i < lineEnd && text[i] == '"') return quoted(text, i + 1, lineEnd);
    const size_t start = i;
    while (i < lineEnd && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ')') ++i;
    return text.substr(start, i - start);
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Just enough JSON to read the pages of an answer: strings (with their escapes),
// numbers, and anything else skipped over.
struct Json {
    const std::string& s;
    size_t i = 0;

    void space() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int n = 0; n < 4 && i < s.size(); ++n, ++i) {
            const char c = s[i];
            v = v * 16 + static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10) & 15);
        }
        return v;
    }
    bool string(std::string& out) {
        out.clear();
        if (!eat('"')) return false;
        while (i < s.size() && s[i] != '"') {
            if (s[i] != '\\') {
                out += s[i++];
                continue;
            }
            if (++i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                case 'b': case 'f': break;
                case 'u': {
                    uint32_t c = hex4();
                    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        const uint32_t low = hex4();
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, c);
                    break;
                }
                default: out += e; break;  // " \ /
            }
        }
        ++i;
        return true;
    }
    void skip() {
        space();
        if (i >= s.size()) return;
        std::string ignored;
        if (s[i] == '"') {
            string(ignored);
        } else if (s[i] == '{' || s[i] == '[') {
            const char close = s[i] == '{' ? '}' : ']';
            ++i;
            while (!eat(close) && i < s.size()) {
                if (close == '}') {
                    string(ignored);
                    eat(':');
                }
                skip();
                eat(',');
            }
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        }
    }
};

const std::set<std::string>& articles() {
    static const std::set<std::string> words = {"the", "a", "an", "el", "la", "los", "las", "le", "les", "o", "os", "de", "of"};
    return words;
}

// The words of a title to compare: without what is in brackets, articles or roman numerals.
std::set<std::string> titleWords(const std::string& title) {
    std::string bare;
    int depth = 0;
    for (char c : title) {
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if (depth == 0) bare += c;
    }
    static const char* const roman[] = {"i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x"};
    std::set<std::string> out;
    const std::string key = searchKey(bare);
    size_t start = 0;
    while (start < key.size()) {
        size_t end = key.find(' ', start);
        if (end == std::string::npos) end = key.size();
        std::string w = key.substr(start, end - start);
        start = end + 1;
        if (articles().count(w)) continue;
        for (int n = 1; n < 10; ++n)  // "i" alone is too often a word
            if (w == roman[n]) w = std::to_string(n + 1);
        out.insert(w);
    }
    return out;
}

bool isNumber(const std::string& w) { return !w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); }); }

}  // namespace

std::vector<std::pair<std::string, std::string>> parseMetadat(const std::string& text, const std::string& field) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string name, value;
    bool inGame = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        size_t a = i;
        while (a < end && (text[a] == ' ' || text[a] == '\t')) ++a;
        size_t b = a;
        while (b < end && !std::isspace(static_cast<unsigned char>(text[b])) && text[b] != '(') ++b;
        const std::string key = text.substr(a, b - a);
        if (key == "game") {
            inGame = true;
            name.clear();
            value.clear();
        } else if (inGame && key == ")") {
            if (!name.empty() && !value.empty()) out.emplace_back(name, value);
            inGame = false;
        } else if (inGame && (key == "comment" || (key == "name" && name.empty()))) {
            name = lineValue(text, b, end);
        } else if (inGame && key == field) {
            value = lineValue(text, b, end);
        }
        i = end + 1;
    }
    return out;
}

void DetailsTable::add(const std::string& field, const std::string& text) {
    // "users" is the key in the maxusers files, the folder's name in the others.
    const std::string key = field == "maxusers" ? "users" : field;
    for (auto& [name, value] : parseMetadat(text, key)) {
        const std::string id = lower(name);
        GameDetails& d = games_[id];
        if (field == "developer") d.developer = value;
        else if (field == "publisher") d.publisher = value;
        else if (field == "genre") d.genre = value;
        else if (field == "releaseyear") d.year = value;
        else if (field == "maxusers") d.players = value;
        byTitle_.emplace(searchKey(gameTitle(name)), id);
    }
}

const GameDetails* DetailsTable::find(const std::string& fileStem) const {
    auto it = games_.find(lower(fileStem));
    if (it != games_.end()) return &it->second;
    auto t = byTitle_.find(searchKey(gameTitle(fileStem)));
    if (t == byTitle_.end()) return nullptr;
    it = games_.find(t->second);
    return it == games_.end() ? nullptr : &it->second;
}

std::vector<WikiPage> parseWikiSearch(const std::string& text) {
    std::vector<WikiPage> out;
    const size_t at = text.find("\"pages\"");
    if (at == std::string::npos) return out;
    Json j{text, at + 7};
    if (!j.eat(':') || !j.eat('[')) return out;
    while (j.i < text.size() && !j.eat(']')) {
        if (!j.eat('{')) {
            j.skip();
            j.eat(',');
            continue;
        }
        WikiPage page;
        std::string key;
        while (j.i < text.size() && !j.eat('}')) {
            if (!j.string(key) || !j.eat(':')) return out;
            if (key == "title") j.string(page.title);
            else if (key == "extract") j.string(page.extract);
            else if (key == "index") {
                j.space();
                page.index = std::atoi(text.c_str() + j.i);
                j.skip();
            } else j.skip();
            j.eat(',');
        }
        if (!page.title.empty()) out.push_back(std::move(page));
        j.eat(',');
    }
    std::stable_sort(out.begin(), out.end(), [](const WikiPage& a, const WikiPage& b) { return a.index < b.index; });
    return out;
}

int wikiTitleDistance(const std::string& gameTitle, const std::string& pageTitle) {
    const std::set<std::string> g = titleWords(gameTitle), p = titleWords(pageTitle);
    if (g.empty() || p.empty()) return -1;
    int shared = 0, onlyGame = 0, onlyPage = 0;
    for (const auto& w : g) {
        if (p.count(w)) ++shared;
        else if (isNumber(w)) return -1;  // "Sonic the Hedgehog" is not "Sonic the Hedgehog 2"
        else ++onlyGame;
    }
    for (const auto& w : p) {
        if (g.count(w)) continue;
        if (isNumber(w)) return -1;
        ++onlyPage;
    }
    if (onlyGame && onlyPage) return -1;
    if (shared * 2 < static_cast<int>(g.size())) return -1;
    return onlyGame + onlyPage;
}

const char* genreName(const std::string& genre) {
    static const std::pair<const char*, const char*> kGenres[] = {
        {"Action", N_("Acción")},
        {"Sports", N_("Deportes")},
        {"Platform", N_("Plataformas")},
        {"Role-playing (RPG)", N_("Rol (RPG)")},
        {"Racing", N_("Carreras")},
        {"Shooter", N_("Disparos")},
        {"Strategy", N_("Estrategia")},
        {"Shoot'em Up", N_("Matamarcianos")},
        {"Beat'em Up", N_("Beat'em up")},
        {"Adventure", N_("Aventura")},
        {"Puzzle", N_("Puzle")},
        {"Fighting", N_("Lucha")},
        {"Simulation", N_("Simulación")},
        {"Quiz", N_("Preguntas")},
        {"Pinball", N_("Pinball")},
        {"Board", N_("Tablero")},
        {"Compilation", N_("Recopilatorio")},
        {"Educational", N_("Educativo")},
        {"Gambling", N_("Juegos de azar")},
        {"Various", N_("Varios")},
        {"Lightgun Shooter", N_("Pistola de luz")},
        {"Hunting and Fishing", N_("Caza y pesca")},
        {"Casual Game", N_("Casual")},
        {"Music / Dancing", N_("Música y baile")},
        {"Sports with Animals", N_("Deportes con animales")},
    };
    for (const auto& [en, es] : kGenres)
        if (genre == en) return es;
    return nullptr;
}

}  // namespace plumnin
