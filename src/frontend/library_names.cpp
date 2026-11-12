#include "library_names.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace plumnin {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Where the first " (" or " [" tag starts, or the end of the name.
size_t tagsStart(const std::string& stem) {
    size_t i = 0;
    while ((i = stem.find_first_of("([", i)) != std::string::npos) {
        if (i > 0) return i;
        ++i;
    }
    return stem.size();
}

// The tags one by one, without their brackets.
std::vector<std::string> tags(const std::string& stem) {
    std::vector<std::string> out;
    size_t i = tagsStart(stem);
    while (i < stem.size()) {
        const char open = stem[i];
        if (open != '(' && open != '[') {
            ++i;
            continue;
        }
        const size_t close = stem.find(open == '(' ? ')' : ']', i);
        if (close == std::string::npos) break;
        out.push_back(stem.substr(i + 1, close - i - 1));
        i = close + 1;
    }
    return out;
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string bareName(const std::string& stem) { return trim(stem.substr(0, tagsStart(stem))); }

std::string libretroSafe(std::string name) {
    for (char& c : name)
        if (std::strchr("&*/:`<>?\\|\"", c)) c = '_';
    return name;
}

}  // namespace

std::string gameTitle(const std::string& fileStem) {
    std::string title = bareName(fileStem);
    // "Lion King, The" and "Adventures of Batman & Robin, The - Subtitle".
    for (const char* article : {", The", ", A", ", An"}) {
        const size_t at = title.find(article);
        if (at == std::string::npos) continue;
        const size_t end = at + std::strlen(article);
        if (end != title.size() && title.compare(end, 3, " - ") != 0) continue;
        title = std::string(article + 2) + " " + title.substr(0, at) + title.substr(end);
        break;
    }
    return title.empty() ? fileStem : title;
}

std::string gameTags(const std::string& fileStem) {
    std::string out;
    for (const auto& t : tags(fileStem)) out += (out.empty() ? "" : " · ") + t;
    return out;
}

std::string searchKey(const std::string& text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            if (space && !out.empty()) out += ' ';
            out += static_cast<char>(std::tolower(c));
            space = false;
        } else {
            space = true;
        }
    }
    return out;
}

std::vector<std::string> coverNames(const std::string& fileStem) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        const std::string safe = libretroSafe(trim(n));
        if (!safe.empty() && std::find(out.begin(), out.end(), safe) == out.end()) out.push_back(safe);
    };
    const std::string bare = bareName(fileStem);
    const std::vector<std::string> t = tags(fileStem);
    // Whole name, then dropping tags from the end: "(USA) (Sega Smash Pack)" -> "(USA)".
    for (size_t keep = t.size() + 1; keep-- > 0;) {
        std::string n = bare;
        for (size_t i = 0; i < keep; ++i) n += " (" + t[i] + ")";
        if (keep > 0) add(n);
    }
    for (const char* region : {"USA", "USA, Europe", "World", "Europe", "Japan, USA", "Japan", "USA, Europe, Korea"})
        add(bare + " (" + region + ")");
    return out;
}

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string htmlDecode(const std::string& text) {
    std::string out = text;
    static const char* const entities[][2] = {{"&amp;", "&"}, {"&#39;", "'"}, {"&quot;", "\""}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (const auto& e : entities)
        for (size_t at; (at = out.find(e[0])) != std::string::npos;) out.replace(at, std::strlen(e[0]), e[1]);
    return out;
}

// A JSON string starting just after its opening quote; `end` gets where it closes.
std::string jsonString(const std::string& text, size_t start, size_t* end) {
    std::string out;
    size_t i = start;
    for (; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        const char c = text[++i];
        if (c == 'u' && i + 4 < text.size()) {
            unsigned cp = 0;
            for (int k = 1; k <= 4; ++k) cp = cp * 16 + static_cast<unsigned>(std::max(0, hexValue(text[i + k])));
            i += 4;
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }
    }
    *end = i;
    return out;
}

// "II" and "2" are the same word; so are "&" and "and" once punctuation is gone.
std::string normalWord(const std::string& w) {
    static const char* const numerals[][2] = {{"ii", "2"}, {"iii", "3"}, {"iv", "4"}, {"v", "5"}, {"vi", "6"}, {"vii", "7"}, {"viii", "8"}};
    for (const auto& n : numerals)
        if (w == n[0]) return n[1];
    return w;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    const std::string key = searchKey(text);
    size_t i = 0;
    while (i < key.size()) {
        size_t space = key.find(' ', i);
        if (space == std::string::npos) space = key.size();
        const std::string w = normalWord(key.substr(i, space - i));
        if (w != "the" && w != "a" && w != "an" && w != "and" && w != "of") out.push_back(w);
        i = space + 1;
    }
    return out;
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
        }
    }
    return row[b.size()];
}

// How well one word of the query matches one word of a name, 0 to 1.
double wordMatch(const std::string& q, const std::string& n) {
    if (q == n) return 1.0;
    const size_t shorter = std::min(q.size(), n.size());
    if (shorter >= 2 && q.compare(0, shorter, n, 0, shorter) == 0) return 0.75;  // "sonic" and "son"
    const size_t longer = std::max(q.size(), n.size());
    if (longer < 4) return 0.0;
    const double similar = 1.0 - static_cast<double>(editDistance(q, n)) / static_cast<double>(longer);
    return similar >= 0.7 ? similar * 0.8 : 0.0;  // "hedgehog" and "hedgehod"
}

}  // namespace

std::vector<std::string> parseCoverList(const std::string& listing) { return parseFileList(listing, ".png"); }

std::vector<std::string> parseFileList(const std::string& listing, const std::string& extension) {
    const std::string ext = lowerCase(extension);
    std::vector<std::string> out;
    const bool json = listing.find("\"path\"") != std::string::npos;
    const std::string marker = json ? "\"path\"" : "href=\"";
    for (size_t at = listing.find(marker); at != std::string::npos; at = listing.find(marker, at + 1)) {
        std::string name;
        if (json) {
            const size_t quote = listing.find('"', listing.find(':', at + marker.size()));
            if (quote == std::string::npos) break;
            size_t end;
            name = jsonString(listing, quote + 1, &end);
        } else {
            const size_t start = at + marker.size();
            const size_t end = listing.find('"', start);
            if (end == std::string::npos) break;
            name = urlDecode(htmlDecode(listing.substr(start, end - start)));
        }
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (name.size() <= ext.size() || lowerCase(name.substr(name.size() - ext.size())) != ext) continue;
        out.push_back(name.substr(0, name.size() - ext.size()));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> similarCoverNames(const std::vector<std::string>& names, const std::string& query, size_t max) {
    const std::vector<std::string> q = words(gameTitle(query));
    if (q.empty() || max == 0) return {};
    std::vector<std::pair<double, size_t>> scored;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::vector<std::string> n = words(gameTitle(names[i]));
        if (n.empty()) continue;
        double sum = 0.0;
        size_t matched = 0;
        for (const std::string& qw : q) {
            double best = 0.0;
            for (const std::string& nw : n) best = std::max(best, wordMatch(qw, nw));
            sum += best;
            matched += best > 0.0;
        }
        if (matched == 0) continue;
        double score = sum / static_cast<double>(q.size());
        // Of two names with the query's words, the one with fewer others is closer.
        score -= 0.04 * static_cast<double>(n.size() > matched ? n.size() - matched : 0);
        if (score < 0.3) continue;
        scored.push_back({score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (size_t i = 0; i < scored.size() && out.size() < max; ++i) out.push_back(names[scored[i].second]);
    return out;
}

std::vector<std::string> cheatFileNames(const std::vector<std::string>& index, const std::string& fileStem, size_t max) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        if (std::binary_search(index.begin(), index.end(), n) && std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    };
    for (const std::string& n : coverNames(fileStem)) {
        add(n);
        add(n + " (Action Replay)");
    }
    // The "(Rumbles)" files are mostly pad vibration settings, not codes: last.
    std::vector<std::string> similar = similarCoverNames(index, fileStem, max);
    std::stable_partition(similar.begin(), similar.end(),
                          [](const std::string& n) { return n.find("(Rumbles)") == std::string::npos; });
    for (const std::string& n : similar) add(n);
    return out;
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace plumnin
