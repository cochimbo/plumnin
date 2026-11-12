#include "screenscraper_data.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>

#include "../core/i18n.h"
#include "library_names.h"

namespace plumnin {

namespace {

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

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

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
            if (++i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': case 'b': case 'f': break;
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
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool value(JsonValue& v) {
        space();
        if (i >= s.size() || ++depth > 64) return false;
        const char c = s[i];
        bool ok = true;
        if (c == '"') {
            v.type = JsonValue::String;
            ok = string(v.text);
        } else if (c == '{' || c == '[') {
            const bool object = c == '{';
            v.type = object ? JsonValue::Object : JsonValue::Array;
            ++i;
            const char close = object ? '}' : ']';
            while (ok && !eat(close)) {
                if (object) {
                    std::string key;
                    if (!string(key) || !eat(':')) {
                        ok = false;
                        break;
                    }
                    v.keys.push_back(std::move(key));
                }
                v.items.emplace_back();
                ok = value(v.items.back());
                if (ok && !eat(',')) ok = eat(close) ? (--i, true) : false;  // the close is eaten next round
            }
        } else {
            const size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            v.text = s.substr(start, i - start);
            if (v.text == "null") v.type = JsonValue::Null, v.text.clear();
            else if (v.text == "true" || v.text == "false") v.type = JsonValue::Bool;
            else if (!v.text.empty() && (std::isdigit(static_cast<unsigned char>(v.text[0])) || v.text[0] == '-')) v.type = JsonValue::Number;
            else ok = false;
        }
        --depth;
        return ok;
    }
};

std::string field(const JsonValue& v) { return v["text"].text; }

// [{"region": "eu", "text": "..."}...] (or "langue" instead of "region").
std::vector<std::pair<std::string, std::string>> localized(const JsonValue& list, const char* key) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const JsonValue& e : list.items)
        if (!e.isObject()) continue;
        else if (const std::string& t = e["text"].text; !t.empty()) out.emplace_back(e[key].text, t);
    return out;
}

std::string best(const std::vector<std::pair<std::string, std::string>>& list, const std::vector<std::string>& prefs) {
    for (const std::string& p : prefs)
        for (const auto& [where, text] : list)
            if (where == p) return text;
    return list.empty() ? std::string() : list.front().second;
}

int number(const std::string& s) { return std::atoi(s.c_str()); }

SsGame readGame(const JsonValue& j) {
    SsGame g;
    g.id = number(j["id"].text);
    g.system = number(j["systeme"]["id"].text);
    g.systemName = field(j["systeme"]);
    g.names = localized(j["noms"], "region");
    g.dates = localized(j["dates"], "region");
    g.synopsis = localized(j["synopsis"], "langue");
    for (const JsonValue& genre : j["genres"].items) {
        auto names = localized(genre["noms"], "langue");
        if (!names.empty()) g.genres.push_back(std::move(names));
    }
    g.publisher = field(j["editeur"]);
    g.developer = field(j["developpeur"]);
    g.players = field(j["joueurs"]);
    g.rating = field(j["note"]);
    for (const JsonValue& m : j["medias"].items) {
        SsMedia media{m["type"].text, m["region"].text, m["format"].text, ssStripCredentials(m["url"].text)};
        if (!media.type.empty() && !media.url.empty()) g.medias.push_back(std::move(media));
    }
    return g;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace

const JsonValue& JsonValue::operator[](const char* key) const {
    static const JsonValue none;
    if (type != Object) return none;
    for (size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == key) return items[k];
    return none;
}

bool parseJson(const std::string& text, JsonValue& out) {
    Parser p{text};
    out = JsonValue();
    if (!p.value(out)) return false;
    p.space();
    return p.i == text.size();
}

std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string ssGameFolder(const std::string& root, int system, const std::string& stem) {
    static const char* const kFolders[] = {"GameBoy", "GameBoyColor", "NES", "SNES"};
    namespace fs = std::filesystem;
    if (root.empty() || system < 0 || system >= static_cast<int>(std::size(kFolders))) return {};
    return (fs::u8path(root) / kFolders[system] / fs::u8path(stem)).u8string() + static_cast<char>(fs::path::preferred_separator);
}

int screenScraperSystem(int system) {
    static const int ids[] = {9, 10, 3, 4};
    return system >= 0 && system < 4 ? ids[system] : 0;
}

std::string SsGame::name(const std::vector<std::string>& regions) const { return best(names, regions); }
std::string SsGame::date(const std::vector<std::string>& regions) const { return best(dates, regions); }
std::string SsGame::text(const std::vector<std::string>& languages) const { return best(synopsis, languages); }
std::string SsGame::genre(const std::vector<std::string>& languages) const {
    std::string out;
    for (const auto& names : genres) {
        const std::string n = best(names, languages);
        if (n.empty() || contains(out, n.c_str())) continue;
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

SsAnswer parseSsAnswer(int httpStatus, const std::string& body) {
    SsAnswer a;
    // The first line of an error answer is the server's message (in French and often English).
    std::string message = body.substr(0, std::min<size_t>(body.size(), 300));
    if (const size_t nl = message.find('\n'); nl != std::string::npos) message.resize(nl);
    a.message = message;
    const std::string low = lower(body.substr(0, 1024));
    if (httpStatus == 0) {
        a.result = SsAnswer::Offline;
        return a;
    }
    JsonValue j;
    const bool json = parseJson(body, j) && j.isObject();
    // A JSON answer is read by its fields: its text (a synopsis, the servers' "threads"
    // counters...) may hold any of the words below. Only its header's error, if it says
    // it failed, is looked at.
    if (httpStatus == 200 && json) {
        const JsonValue& header = j["header"];
        if (header["success"].text != "false") {
            a.result = SsAnswer::Ok;
            a.message.clear();
            const JsonValue& response = j["response"];
            const JsonValue& user = response["ssuser"];
            if (user.isObject()) {
                if (!user["requeststoday"].text.empty()) a.requestsToday = number(user["requeststoday"].text);
                if (!user["maxrequestsperday"].text.empty()) a.maxRequests = number(user["maxrequestsperday"].text);
            }
            if (response["jeu"].isObject()) {
                SsGame g = readGame(response["jeu"]);
                if (g.id) a.games.push_back(std::move(g));
            }
            for (const JsonValue& e : response["jeux"].items) {
                if (!e.isObject()) continue;
                SsGame g = readGame(e);
                if (g.id) a.games.push_back(std::move(g));  // an empty search answers [{}]
            }
            if (a.games.empty()) a.result = SsAnswer::NotFound;
            return a;
        }
        return parseSsAnswer(500, header["error"].text.empty() ? std::string("error") : header["error"].text);
    }
    if (httpStatus == 404 || contains(low, "non trouv")) a.result = SsAnswer::NotFound;
    else if (httpStatus == 403 || contains(low, "erreur de login") || contains(low, "identifiants")) a.result = SsAnswer::Login;
    else if (httpStatus == 430 || httpStatus == 431 || contains(low, "quota")) a.result = SsAnswer::Quota;
    else if (httpStatus == 423 || contains(low, "totalement ferm")) a.result = SsAnswer::Closed;
    else if (httpStatus == 401 || httpStatus == 429 || contains(low, "non membres") || contains(low, "non-registered") ||
             contains(low, "threads"))
        a.result = SsAnswer::Busy;
    else if (httpStatus != 200 || !json) a.result = SsAnswer::Error;
    else a.result = SsAnswer::Ok;
    if (!json) return a;
    a.message.clear();
    const JsonValue& response = j["response"];
    const JsonValue& user = response["ssuser"];
    if (user.isObject()) {
        if (!user["requeststoday"].text.empty()) a.requestsToday = number(user["requeststoday"].text);
        if (!user["maxrequestsperday"].text.empty()) a.maxRequests = number(user["maxrequestsperday"].text);
    }
    if (response["jeu"].isObject()) {
        SsGame g = readGame(response["jeu"]);
        if (g.id) a.games.push_back(std::move(g));
    }
    for (const JsonValue& e : response["jeux"].items) {
        if (!e.isObject()) continue;
        SsGame g = readGame(e);
        if (g.id) a.games.push_back(std::move(g));  // an empty search answers [{}]
    }
    if (a.result == SsAnswer::Ok && a.games.empty()) a.result = SsAnswer::NotFound;
    return a;
}

std::string ssGameJson(const SsGame& g) {
    auto list = [](const std::vector<std::pair<std::string, std::string>>& l, const char* key) {
        std::string out = "[";
        for (size_t k = 0; k < l.size(); ++k)
            out += (k ? ",{" : "{") + jsonQuote(key) + ":" + jsonQuote(l[k].first) + ",\"text\":" + jsonQuote(l[k].second) + "}";
        return out + "]";
    };
    auto text = [](const std::string& t) { return "{\"text\":" + jsonQuote(t) + "}"; };
    std::string out = "{\"response\":{\"jeu\":{\n";
    out += "\"id\":" + jsonQuote(std::to_string(g.id)) + ",\n";
    out += "\"systeme\":{\"id\":" + jsonQuote(std::to_string(g.system)) + ",\"text\":" + jsonQuote(g.systemName) + "},\n";
    out += "\"noms\":" + list(g.names, "region") + ",\n";
    out += "\"dates\":" + list(g.dates, "region") + ",\n";
    out += "\"synopsis\":" + list(g.synopsis, "langue") + ",\n";
    out += "\"genres\":[";
    for (size_t k = 0; k < g.genres.size(); ++k) out += (k ? ",{\"noms\":" : "{\"noms\":") + list(g.genres[k], "langue") + "}";
    out += "],\n";
    out += "\"editeur\":" + text(g.publisher) + ",\n";
    out += "\"developpeur\":" + text(g.developer) + ",\n";
    out += "\"joueurs\":" + text(g.players) + ",\n";
    out += "\"note\":" + text(g.rating) + ",\n";
    out += "\"medias\":[";
    for (size_t k = 0; k < g.medias.size(); ++k) {
        const SsMedia& m = g.medias[k];
        out += std::string(k ? ",\n" : "\n") + "{\"type\":" + jsonQuote(m.type) + ",\"region\":" + jsonQuote(m.region) +
               ",\"format\":" + jsonQuote(m.format) + ",\"url\":" + jsonQuote(m.url) + "}";
    }
    out += "]\n}}}\n";
    return out;
}

const std::vector<SsKind>& ssKinds() {
    static const std::vector<SsKind> kinds = {
        {"portada", N_("Portada"), {"box-2D", nullptr, nullptr}, true, true},
        {"portada-completa", N_("Portada completa"), {"box-texture", nullptr, nullptr}, true, true},
        {"contraportada", N_("Contraportada"), {"box-2D-back", nullptr, nullptr}, true, true},
        {"caja-3d", N_("Caja en 3D"), {"box-3D", nullptr, nullptr}, true, true},
        {"etiqueta", N_("Etiqueta del cartucho"), {"support-texture", nullptr, nullptr}, true, true},
        {"cartucho", N_("Cartucho"), {"support-2D", nullptr, nullptr}, true, true},
        {"captura", N_("Captura"), {"ss", nullptr, nullptr}, true, true},
        {"titulo", N_("Pantalla de título"), {"sstitle", nullptr, nullptr}, true, true},
        {"logo", N_("Logo"), {"wheel-hd", "wheel", nullptr}, true, true},
        {"marquesina", N_("Marquesina"), {"screenmarquee", "screenmarqueesmall", nullptr}, true, true},
        {"fanart", N_("Fondo (fan art)"), {"fanart", nullptr, nullptr}, true, true},
        {"mezcla", N_("Composición"), {"mixrbv2", "mixrbv1", nullptr}, true, false},
        {"manual", N_("Manual"), {"manuel", nullptr, nullptr}, false, false},
        {"video", N_("Vídeo"), {"video-normalized", "video", nullptr}, false, false},
    };
    return kinds;
}

const SsKind* ssKind(const std::string& id) {
    for (const SsKind& k : ssKinds())
        if (id == k.id) return &k;
    return nullptr;
}

std::vector<std::string> ssRegions(const std::string& tags) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"europe", "eu"}, {"usa", "us"}, {"japan", "jp"}, {"world", "wor"}, {"brazil", "br"}, {"korea", "kr"},
        {"france", "fr"}, {"germany", "de"}, {"spain", "sp"}, {"italy", "it"}, {"australia", "au"},
        {"asia", "asi"}, {"china", "cn"}, {"taiwan", "tw"}, {"canada", "ca"}, {"netherlands", "nl"}, {"sweden", "se"},
        {"united kingdom", "uk"}, {"portugal", "pt"}};
    std::vector<std::string> out;
    // In the order the tags name them.
    const std::string low = lower(tags);
    std::vector<std::pair<size_t, const char*>> found;
    for (const auto& [name, code] : kNames)
        if (const size_t at = low.find(name); at != std::string::npos) found.emplace_back(at, code);
    std::sort(found.begin(), found.end());
    for (const auto& f : found) out.push_back(f.second);
    // A European game comes in the box of its own country before any other.
    for (const char* r : {"wor", "eu", "us", "jp", "ss", ""})
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    return out;
}

std::vector<std::string> ssLanguages(const std::string& ui) {
    std::vector<std::string> out;
    if (!ui.empty()) out.push_back(ui);
    for (const char* l : {"en", "fr", "es", "pt", "de", "it"})
        if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
    return out;
}

const SsMedia* ssPickMedia(const SsGame& game, const SsKind& kind, const std::vector<std::string>& regions) {
    for (const char* type : kind.types) {
        if (!type) break;
        for (const std::string& r : regions)
            for (const SsMedia& m : game.medias)
                if (m.type == type && m.region == r) return &m;
        for (const SsMedia& m : game.medias)
            if (m.type == type) return &m;
    }
    return nullptr;
}

std::string ssUrl(const SsCredentials& c, const std::string& endpoint, const std::string& params) {
    std::string url = "https://api.screenscraper.fr/api2/" + endpoint + "?devid=" + urlEncode(c.devId) +
                      "&devpassword=" + urlEncode(c.devPassword) + "&softname=" + urlEncode(c.softName) + "&output=json";
    if (!c.user.empty() && !c.password.empty())
        url += "&ssid=" + urlEncode(c.user) + "&sspassword=" + urlEncode(c.password);
    if (!params.empty()) url += "&" + params;
    return url;
}

std::string ssStripCredentials(const std::string& url) {
    const size_t q = url.find('?');
    if (q == std::string::npos) return url;
    std::string out = url.substr(0, q), sep = "?";
    size_t i = q + 1;
    while (i <= url.size()) {
        size_t end = url.find('&', i);
        if (end == std::string::npos) end = url.size();
        const std::string param = url.substr(i, end - i);
        const std::string key = param.substr(0, param.find('='));
        if (!param.empty() && key != "devid" && key != "devpassword" && key != "softname" && key != "ssid" && key != "sspassword") {
            out += sep + param;
            sep = "&";
        }
        i = end + 1;
    }
    return out;
}

std::string ssWithCredentials(const std::string& url, const SsCredentials& c) {
    std::string bare = ssStripCredentials(url);
    const size_t q = bare.find('?');
    const std::string base = q == std::string::npos ? bare : bare.substr(0, q);
    const std::string rest = q == std::string::npos ? std::string() : bare.substr(q + 1);
    // Only ScreenScraper's own addresses get the accounts.
    if (base.find("screenscraper.fr/") == std::string::npos) return url;
    std::string out = base + "?devid=" + urlEncode(c.devId) + "&devpassword=" + urlEncode(c.devPassword) +
                      "&softname=" + urlEncode(c.softName);
    if (!c.user.empty() && !c.password.empty())
        out += "&ssid=" + urlEncode(c.user) + "&sspassword=" + urlEncode(c.password);
    if (!rest.empty()) out += "&" + rest;
    return out;
}

std::string ssRomParams(int system, const std::string& fileName, unsigned long long size, unsigned crc, bool disc) {
    std::string p = "systemeid=" + std::to_string(screenScraperSystem(system)) + "&romtype=" + (disc ? "iso" : "rom") +
                    "&romnom=" + urlEncode(fileName);
    if (size) p += "&romtaille=" + std::to_string(size);
    if (crc) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", crc);
        p += std::string("&crc=") + buf;
    }
    return p;
}

}  // namespace plumnin
